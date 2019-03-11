/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "uuid_catalog.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {
const ServiceContext::Decoration<UUIDCatalog> getCatalog =
    ServiceContext::declareDecoration<UUIDCatalog>();
}  // namespace

void UUIDCatalogObserver::onCreateCollection(OperationContext* opCtx,
                                             Collection* coll,
                                             const NamespaceString& collectionName,
                                             const CollectionOptions& options,
                                             const BSONObj& idIndex,
                                             const OplogSlot& createOpTime) {
    if (!options.uuid)
        return;
    UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
    catalog.onCreateCollection(opCtx, coll, options.uuid.get());
}

void UUIDCatalogObserver::onCollMod(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    OptionalCollectionUUID uuid,
                                    const BSONObj& collModCmd,
                                    const CollectionOptions& oldCollOptions,
                                    boost::optional<TTLCollModInfo> ttlInfo) {
    if (!uuid)
        return;
    UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
    Collection* catalogColl = catalog.lookupCollectionByUUID(uuid.get());
    invariant(
        catalogColl->uuid() == uuid,
        str::stream() << (uuid ? uuid->toString() : "<no uuid>") << ","
                      << (catalogColl->uuid() ? catalogColl->uuid()->toString() : "<no uuid>"));
}

repl::OpTime UUIDCatalogObserver::onDropCollection(OperationContext* opCtx,
                                                   const NamespaceString& collectionName,
                                                   OptionalCollectionUUID uuid,
                                                   std::uint64_t numRecords,
                                                   const CollectionDropType dropType) {

    if (!uuid)
        return {};

    // Replicated drops are two-phase, meaning that the collection is first renamed into a "drop
    // pending" state and reaped later. This op observer is only called for the rename phase, which
    // means the UUID mapping is still valid.
    //
    // On the other hand, if the drop is not replicated, it takes effect immediately. In this case,
    // the UUID mapping must be removed from the UUID catalog.
    if (dropType == CollectionDropType::kOnePhase) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onDropCollection(opCtx, uuid.get());
    }

    return {};
}

UUIDCatalog::iterator::iterator(std::string dbName, uint64_t genNum, const UUIDCatalog& uuidCatalog)
    : _dbName(dbName), _genNum(genNum), _uuidCatalog(&uuidCatalog) {
    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    _mapIter = _uuidCatalog->_orderedCollections.lower_bound(std::make_pair(_dbName, minUuid));
    if (_mapIter != _uuidCatalog->_orderedCollections.end()) {
        _uuid = _mapIter->first.second;
    }
}

UUIDCatalog::iterator::iterator(
    std::map<std::pair<std::string, CollectionUUID>, UUIDCatalog::CollectionInfo*>::const_iterator mapIter)
    : _mapIter(mapIter) {}

UUIDCatalog::iterator::pointer UUIDCatalog::iterator::operator->() {
    stdx::lock_guard<stdx::mutex> lock(_uuidCatalog->_catalogLock);
    _repositionIfNeeded();
    if (_exhausted()) {
        return nullptr;
    }

    return &_mapIter->second->coll;
}

UUIDCatalog::iterator::reference UUIDCatalog::iterator::operator*() {
    stdx::lock_guard<stdx::mutex> lock(_uuidCatalog->_catalogLock);
    _repositionIfNeeded();
    if (_exhausted()) {
        return _nullCollection;
    }

    return _mapIter->second->coll;
}

CollectionCatalogEntry* UUIDCatalog::iterator::getCollectionCatalogEntry() {
    stdx::lock_guard<stdx::mutex> lock(_uuidCatalog->_catalogLock);
    _repositionIfNeeded();
    if (_exhausted()) {
        return _nullCatalogEntry;
    }

    auto uuid = *(_mapIter->second->coll->uuid());
    auto catalogIt = _uuidCatalog->_catalog.find(uuid);
    invariant(catalogIt != _uuidCatalog->_catalog.end());

    return catalogIt->second->collCatalogEntry.get();
}

UUIDCatalog::iterator UUIDCatalog::iterator::operator++() {
    stdx::lock_guard<stdx::mutex> lock(_uuidCatalog->_catalogLock);

    if (!_repositionIfNeeded()) {
        _mapIter++;  // If the position was not updated, increment iterator to next element.
    }

    if (_exhausted()) {
        // If the iterator is at the end of the map or now points to an entry that does not
        // correspond to the correct database.
        _mapIter = _uuidCatalog->_orderedCollections.end();
        _uuid = boost::none;
        return *this;
    }

    _uuid = _mapIter->first.second;
    return *this;
}

UUIDCatalog::iterator UUIDCatalog::iterator::operator++(int) {
    auto oldPosition = *this;
    ++(*this);
    return oldPosition;
}

bool UUIDCatalog::iterator::operator==(const iterator& other) {
    stdx::lock_guard<stdx::mutex> lock(_uuidCatalog->_catalogLock);

    if (other._mapIter == _uuidCatalog->_orderedCollections.end()) {
        return _uuid == boost::none;
    }

    return _uuid == other._uuid;
}

bool UUIDCatalog::iterator::operator!=(const iterator& other) {
    return !(*this == other);
}

// Check if _mapIter has been invalidated due to a change in the _orderedCollections map. If it
// has, restart iteration through a call to lower_bound. If the element that the iterator is
// currently pointing to has been deleted, the iterator will be repositioned to the element that
// followed it.
bool UUIDCatalog::iterator::_repositionIfNeeded() {
    // If the generation number has changed, the _orderedCollections map has been modified in a
    // way that could possibly invalidate this iterator. In this case, try to find the same
    // entry the iterator was on, or the one right after it.

    if (_genNum == _uuidCatalog->_generationNumber) {
        return false;
    }

    _genNum = _uuidCatalog->_generationNumber;
    _mapIter = _uuidCatalog->_orderedCollections.lower_bound(std::make_pair(_dbName, *_uuid));

    if (_exhausted()) {
        // The deleted entry was the final one for this database and the iterator has been
        // repositioned.
        return true;
    }

    // If the old pair matches the previous DB name and UUID, the iterator was not repositioned.
    auto dbUuidPair = _mapIter->first;
    return !(dbUuidPair.first == _dbName && dbUuidPair.second == _uuid);
}

bool UUIDCatalog::iterator::_exhausted() {
    return _mapIter == _uuidCatalog->_orderedCollections.end() || _mapIter->first.first != _dbName;
}

UUIDCatalog& UUIDCatalog::get(ServiceContext* svcCtx) {
    return getCatalog(svcCtx);
}
UUIDCatalog& UUIDCatalog::get(OperationContext* opCtx) {
    return getCatalog(opCtx->getServiceContext());
}

void UUIDCatalog::onCreateCollection(OperationContext* opCtx,
                                     Collection* coll,
                                     CollectionUUID uuid) {

    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    _removeUUIDCatalogEntry_inlock(uuid);  // Remove UUID if it exists
    _registerUUIDCatalogEntry_inlock(uuid, coll);
    opCtx->recoveryUnit()->onRollback([this, uuid] { removeUUIDCatalogEntry(uuid); });
}

void UUIDCatalog::onDropCollection(OperationContext* opCtx, CollectionUUID uuid) {
    Collection* foundColl = removeUUIDCatalogEntry(uuid);
    opCtx->recoveryUnit()->onRollback(
        [this, foundColl, uuid] { registerUUIDCatalogEntry(uuid, foundColl); });
}

void UUIDCatalog::setCollectionNamespace(OperationContext* opCtx,
                                         Collection* coll,
                                         const NamespaceString& fromCollection,
                                         const NamespaceString& toCollection) {
    // Rather than maintain, in addition to the UUID -> Collection* mapping, an auxiliary data
    // structure with the UUID -> namespace mapping, the UUIDCatalog relies on Collection::ns() to
    // provide UUID to namespace lookup. In addition, the UUIDCatalog does not require callers to
    // hold locks.
    //
    // This means that Collection::ns() may be called while only '_catalogLock' (and no lock manager
    // locks) are held. The purpose of this function is ensure that we write to the Collection's
    // namespace string under '_catalogLock'.
    invariant(coll);
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    coll->setNs(toCollection);

    auto it = _collections.find(fromCollection);
    invariant(it != _collections.end());
    auto collInfo = it->second;
    _collections.erase(it);
    auto entry = std::make_pair(toCollection, collInfo);
    invariant(_collections.insert(entry).second == true);

    opCtx->recoveryUnit()->onRollback([this, coll, fromCollection, toCollection] {
        stdx::lock_guard<stdx::mutex> lock(_catalogLock);
        coll->setNs(std::move(fromCollection));
        auto it = _collections.find(toCollection);
        auto collInfo = it->second;
        _collections.erase(it);
        auto collEntry = std::make_pair(fromCollection, collInfo);
        _collections.insert(collEntry);
    });
}

void UUIDCatalog::onCloseDatabase(Database* db) {
    for (auto&& coll : *db) {
        if (coll->uuid()) {
            // While the collection does not actually get dropped, we're going to destroy the
            // Collection object, so for purposes of the UUIDCatalog it looks the same.
            removeUUIDCatalogEntry(coll->uuid().get());
        }
    }
}

void UUIDCatalog::onCloseCatalog(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    invariant(!_shadowCatalog);
    _shadowCatalog.emplace();
    for (auto& entry : _catalog)
        _shadowCatalog->insert({entry.first, entry.second->coll->ns()});
}

void UUIDCatalog::onOpenCatalog(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    invariant(_shadowCatalog);
    _shadowCatalog.reset();
}

Collection* UUIDCatalog::lookupCollectionByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    return foundIt == _catalog.end() ? nullptr : foundIt->second->coll;
}

Collection* UUIDCatalog::lookupCollectionByNamespace(const NamespaceString& nss) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto it = _collections.find(nss);
    return it == _collections.end() ? nullptr : it->second->coll;
}

CollectionCatalogEntry* UUIDCatalog::lookupCollectionCatalogEntryByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    return foundIt == _catalog.end() ? nullptr : foundIt->second->collCatalogEntry.get();
}

CollectionCatalogEntry* UUIDCatalog::lookupCollectionCatalogEntryByNamespace(
    const NamespaceString& nss) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto collIt = _collections.find(nss);

    return collIt == _collections.end() ? nullptr : collIt->second->collCatalogEntry.get();
}

void UUIDCatalog::removeCollectionCatalogEntryByNamespace(const NamespaceString& nss) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto collIt = _collections.find(nss);
    invariant(collIt != _collections.end());

    auto uuid = *(collIt->second->coll->uuid());
    auto catalogIt = _catalog.find(uuid);
    invariant(catalogIt != _catalog.end());

    catalogIt->second->collCatalogEntry.reset();
}

NamespaceString UUIDCatalog::lookupNSSByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    if (foundIt != _catalog.end())
        return foundIt->second->coll->ns();

    // Only in the case that the catalog is closed and a UUID is currently unknown, resolve it
    // using the pre-close state. This ensures that any tasks reloading the catalog can see their
    // own updates.
    if (_shadowCatalog) {
        auto shadowIt = _shadowCatalog->find(uuid);
        if (shadowIt != _shadowCatalog->end())
            return shadowIt->second;
    }
    return NamespaceString();
}

Collection* UUIDCatalog::replaceUUIDCatalogEntry(CollectionUUID uuid, Collection* coll) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    invariant(coll);
    Collection* oldColl = _removeUUIDCatalogEntry_inlock(uuid);
    invariant(oldColl != nullptr);  // Need to replace an existing coll
    _registerUUIDCatalogEntry_inlock(uuid, coll);
    return oldColl;
}
void UUIDCatalog::registerUUIDCatalogEntry(CollectionUUID uuid, Collection* coll) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    _registerUUIDCatalogEntry_inlock(uuid, coll);
}

Collection* UUIDCatalog::removeUUIDCatalogEntry(CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    return _removeUUIDCatalogEntry_inlock(uuid);
}

boost::optional<CollectionUUID> UUIDCatalog::prev(StringData db, CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto dbIdPair = std::make_pair(db.toString(), uuid);
    auto entry = _orderedCollections.find(dbIdPair);

    // If the element does not appear or is the first element.
    if (entry == _orderedCollections.end() || entry == _orderedCollections.begin()) {
        return boost::none;
    }

    auto prevEntry = std::prev(entry, 1);
    // If the entry is from a different database, there is no previous entry.
    if (prevEntry->first.first != db) {
        return boost::none;
    }
    return prevEntry->first.second;
}

boost::optional<CollectionUUID> UUIDCatalog::next(StringData db, CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto dbIdPair = std::make_pair(db.toString(), uuid);
    auto entry = _orderedCollections.find(dbIdPair);

    // If the element does not appear.
    if (entry == _orderedCollections.end()) {
        return boost::none;
    }

    auto nextEntry = std::next(entry, 1);
    // If the element was the last entry or is from a different database.
    if (nextEntry == _orderedCollections.end() || nextEntry->first.first != db) {
        return boost::none;
    }
    return nextEntry->first.second;
}

void UUIDCatalog::insertCollectionCatalogEntry(CollectionUUID uuid,
                                               std::unique_ptr<CollectionCatalogEntry> entry) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);

    // If CollectionInfo already exists for this UUID, just set the catalog entry pointer.
    if (foundIt != _catalog.end()) {
        foundIt->second.get()->collCatalogEntry = std::move(entry);
        return;
    }

    auto collInfo = std::make_unique<UUIDCatalog::CollectionInfo>();
    collInfo->collCatalogEntry = std::move(entry);

    auto collNs = collInfo->collCatalogEntry->ns();
    auto dbName = collNs.db();
    auto orderedPair = std::make_pair(std::make_pair(dbName.toString(), uuid), collInfo.get());
    invariant(_orderedCollections.insert(orderedPair).second == true);

    auto collectionsPair = std::make_pair(collNs, collInfo.get());
    invariant(_collections.insert(collectionsPair).second == true);

    invariant(_catalog.insert(std::make_pair(uuid, std::move(collInfo))).second == true);
}

void UUIDCatalog::renameCollectionCatalogEntry(OperationContext* opCtx,
                                               const NamespaceString& fromNss,
                                               const NamespaceString& toNss) {
    // No mappings need to change because the mapping in _collections will change in
    // UUIDCatalog::setCollectionNamespace() and the UUID of the collection will not change so
    // the _catalog and _orderedCollections maps will not be modified.

    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto fromIt = _collections.find(fromNss);
    invariant(fromIt != _collections.end());
    auto uuid = fromIt->second->coll->uuid();

    auto fromCatalogIt = _catalog.find(*uuid);
    invariant(fromCatalogIt != _catalog.end());
    fromCatalogIt->second->collCatalogEntry->setNs(toNss);

    opCtx->recoveryUnit()->onRollback([this, uuid, fromNss] {
        stdx::lock_guard<stdx::mutex> lock(_catalogLock);
        auto it = _catalog.find(*uuid);
        it->second->collCatalogEntry->setNs(fromNss);
    });
}

void UUIDCatalog::_registerUUIDCatalogEntry_inlock(CollectionUUID uuid, Collection* coll) {
    // Collection is invalid or this UUID is already taken.
    if (!coll) {
        return;
    }
    auto catalogIt = _catalog.find(uuid);
    if (catalogIt->second->coll) {
        return;
    }

    LOG(2) << "registering collection " << coll->ns() << " with UUID " << uuid;

    // At this point, the corresponding CollectionCatalogEntry has already been registered so
    // all maps are pointing to a CollectionInfo that is missing a Collection.
    invariant(catalogIt != _catalog.end());
    catalogIt->second->coll = coll;
}
Collection* UUIDCatalog::_removeUUIDCatalogEntry_inlock(CollectionUUID uuid) {
    auto foundIt = _catalog.find(uuid);
    if (foundIt == _catalog.end()) {
        return nullptr;
    }

    // A Collection object might not exist even though the UUID has already been registered because
    // the UUID gets inserted with a CollectionInfo pointing to a null Collection.
    auto foundColl = foundIt->second->coll;
    if (!foundColl) {
        return nullptr;
    }

    LOG(2) << "unregistering collection " << foundColl->ns() << " with UUID " << uuid;
    foundIt->second->coll = nullptr;

    // Removal from an ordered map will invalidate iterators and potentially references to the
    // references to the erased element.
    _generationNumber++;

    return foundColl;
}

UUIDCatalog::iterator UUIDCatalog::begin(StringData db) const {
    return iterator(db.toString(), _generationNumber, *this);
}

UUIDCatalog::iterator UUIDCatalog::end() const {
    return iterator(_orderedCollections.end());
}

}  // namespace mongo
