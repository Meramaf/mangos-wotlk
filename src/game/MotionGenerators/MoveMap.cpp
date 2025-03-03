/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Log.h"
#include "World/World.h"
#include "Entities/Creature.h"
#include "MotionGenerators/MoveMap.h"
#include "MoveMapSharedDefines.h"

namespace MMAP
{
    // ######################## MMapFactory ########################
    // our global singelton copy
    MMapManager* g_MMapManager = nullptr;

    // stores list of mapids which do not use pathfinding
    std::set<uint32>* g_mmapDisabledIds = nullptr;

    MMapManager* MMapFactory::createOrGetMMapManager()
    {
        if (g_MMapManager == nullptr)
            g_MMapManager = new MMapManager();

        return g_MMapManager;
    }

    void MMapFactory::preventPathfindingOnMaps(const char* ignoreMapIds)
    {
        if (!g_mmapDisabledIds)
            g_mmapDisabledIds = new std::set<uint32>();

        uint32 strLenght = strlen(ignoreMapIds) + 1;
        char* mapList = new char[strLenght];
        memcpy(mapList, ignoreMapIds, sizeof(char)*strLenght);

        char* idstr = strtok(mapList, ",");
        while (idstr)
        {
            g_mmapDisabledIds->insert(uint32(atoi(idstr)));
            idstr = strtok(nullptr, ",");
        }

        delete[] mapList;
    }

    bool MMapFactory::IsPathfindingEnabled(uint32 mapId, const Unit* unit = nullptr)
    {
        if (!sWorld.getConfig(CONFIG_BOOL_MMAP_ENABLED))
            return false;

        if (unit)
        {
            // always use mmaps for players
            if (unit->GetTypeId() == TYPEID_PLAYER)
                return true;

            if (IsPathfindingForceDisabled(unit))
                return false;

            if (IsPathfindingForceEnabled(unit))
                return true;

            // always use mmaps for pets of players (can still be disabled by extra-flag for pet creature)
            if (unit->GetTypeId() == TYPEID_UNIT && ((Creature*)unit)->IsPet() && unit->GetOwner() &&
                    unit->GetOwner()->GetTypeId() == TYPEID_PLAYER)
                return true;
        }

        return g_mmapDisabledIds->find(mapId) == g_mmapDisabledIds->end();
    }

    void MMapFactory::clear()
    {
        delete g_mmapDisabledIds;
        delete g_MMapManager;

        g_mmapDisabledIds = nullptr;
        g_MMapManager = nullptr;
    }

    bool MMapFactory::IsPathfindingForceEnabled(const Unit* unit)
    {
        if (const Creature* pCreature = dynamic_cast<const Creature*>(unit))
        {
            if (const CreatureInfo* pInfo = pCreature->GetCreatureInfo())
            {
                if (pInfo->ExtraFlags & CREATURE_EXTRA_FLAG_MMAP_FORCE_ENABLE)
                    return true;
            }
        }

        return false;
    }

    bool MMapFactory::IsPathfindingForceDisabled(const Unit* unit)
    {
        if (const Creature* pCreature = dynamic_cast<const Creature*>(unit))
            return pCreature->IsIgnoringMMAP();

        return false;
    }

    // ######################## MMapManager ########################
    MMapManager::~MMapManager()
    {
        for (auto& loadedMMap : m_loadedMMaps)
            delete loadedMMap.second;

        // by now we should not have maps loaded
        // if we had, tiles in MMapData->mmapLoadedTiles, their actual data is lost!
    }

    void MMapManager::ChangeTile(uint32 mapId, uint32 instanceId, uint32 tileX, uint32 tileY, uint32 tileNumber)
    {
        unloadMap(mapId, instanceId, tileX, tileY);
        loadMap(mapId, instanceId, tileX, tileY, tileNumber);
    }

    bool MMapManager::loadMapData(uint32 mapId, uint32 instanceId)
    {
        // we already have this map loaded?
        if (m_loadedMMaps.find(packInstanceId(mapId, instanceId)) != m_loadedMMaps.end())
            return true;

        // load and init dtNavMesh - read parameters from file
        uint32 pathLen = sWorld.GetDataPath().length() + strlen("mmaps/%03i.mmap") + 1;
        char* fileName = new char[pathLen];
        snprintf(fileName, pathLen, (sWorld.GetDataPath() + "mmaps/%03i.mmap").c_str(), mapId);

        FILE* file = fopen(fileName, "rb");
        if (!file)
        {
            if (MMapFactory::IsPathfindingEnabled(mapId))
                sLog.outError("MMAP:loadMapData: Error: Could not open mmap file '%s'", fileName);
            delete[] fileName;
            return false;
        }

        dtNavMeshParams params;
        fread(&params, sizeof(dtNavMeshParams), 1, file);
        fclose(file);

        dtNavMesh* mesh = dtAllocNavMesh();
        MANGOS_ASSERT(mesh);
        dtStatus dtResult = mesh->init(&params);
        if (dtStatusFailed(dtResult))
        {
            dtFreeNavMesh(mesh);
            sLog.outError("MMAP:loadMapData: Failed to initialize dtNavMesh for mmap %03u from file %s", mapId, fileName);
            delete[] fileName;
            return false;
        }

        delete[] fileName;

        DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:loadMapData: Loaded %03i.mmap", mapId);

        // store inside our map list
        MMapData* mmap_data = new MMapData(mesh);
        mmap_data->mmapLoadedTiles.clear();

        m_loadedMMaps.emplace(packInstanceId(mapId, instanceId), mmap_data);
        return true;
    }

    uint32 MMapManager::packTileID(int32 x, int32 y) const
    {
        return uint32(x << 16 | y);
    }

    uint64 MMapManager::packInstanceId(uint32 mapId, uint32 instanceId) const
    {
        return (uint64(mapId) << 32) | instanceId;
    }

    bool MMapManager::IsMMapTileLoaded(uint32 mapId, uint32 instanceId, uint32 x, uint32 y) const
    {
        // get this mmap data
        auto itr = m_loadedMMaps.find(packInstanceId(mapId, instanceId));

        if (itr == m_loadedMMaps.end())
            return false;

        auto mmap = itr->second;

        uint32 packedGridPos = packTileID(x, y);
        if (mmap->mmapLoadedTiles.find(packedGridPos) != mmap->mmapLoadedTiles.end())
            return true;

        return false;
    }

    bool MMapManager::loadMap(uint32 mapId, uint32 instanceId, int32 x, int32 y, uint32 number)
    {
        // make sure the mmap is loaded and ready to load tiles
        if (!loadMapData(mapId, instanceId))
            return false;

        // get this mmap data
        MMapData* mmap = m_loadedMMaps[packInstanceId(mapId, instanceId)];
        MANGOS_ASSERT(mmap->navMesh);

        char fileName[100];
        if (number == 0)
            sprintf(fileName, "%03u%02i%02i.mmtile", mapId, x, y);
        else
            sprintf(fileName, "%03u%02i%02i_%02i.mmtile", mapId, x, y, number);

        // check if we already have this tile loaded
        uint32 packedGridPos = packTileID(x, y);
        if (mmap->mmapLoadedTiles.find(packedGridPos) != mmap->mmapLoadedTiles.end())
        {
            sLog.outError("MMAP:loadMap: Asked to load already loaded navmesh tile. ");
            return false;
        }

        std::string filePath = sWorld.GetDataPath() + std::string("mmaps/") + fileName;
        // load this tile
        FILE* file = fopen(filePath.c_str(), "rb");
        if (!file)
        {
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "ERROR: MMAP:loadMap: Could not open mmtile file '%s'", fileName);
            return false;
        }

        // read header
        MmapTileHeader fileHeader;
        fread(&fileHeader, sizeof(MmapTileHeader), 1, file);

        if (fileHeader.mmapMagic != MMAP_MAGIC)
        {
            sLog.outError("MMAP:loadMap: Bad header in mmap %s", fileName);
            fclose(file);
            return false;
        }

        if (fileHeader.mmapVersion != MMAP_VERSION)
        {
            sLog.outError("MMAP:loadMap: %s was built with generator v%i, expected v%i",
                          fileName, fileHeader.mmapVersion, MMAP_VERSION);
            fclose(file);
            return false;
        }

        unsigned char* data = (unsigned char*)dtAlloc(fileHeader.size, DT_ALLOC_PERM);
        MANGOS_ASSERT(data);

        size_t result = fread(data, fileHeader.size, 1, file);
        if (!result)
        {
            sLog.outError("MMAP:loadMap: Bad header or data in mmap %s", fileName);
            fclose(file);
            return false;
        }

        fclose(file);

        dtMeshHeader* header = (dtMeshHeader*)data;
        dtTileRef tileRef = 0;

        // memory allocated for data is now managed by detour, and will be deallocated when the tile is removed
        dtStatus dtResult = mmap->navMesh->addTile(data, fileHeader.size, DT_TILE_FREE_DATA, 0, &tileRef);
        if (dtStatusFailed(dtResult))
        {
            sLog.outError("MMAP:loadMap: Could not load %s into navmesh", fileName);
            dtFree(data);
            return false;
        }

        mmap->mmapLoadedTiles.insert(std::pair<uint32, dtTileRef>(packedGridPos, tileRef));
        ++m_loadedTiles;
        DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:loadMap: Loaded into %03i[%02i,%02i]", fileName, mapId, header->x, header->y);
        return true;
    }

    void MMapManager::loadAllGameObjectModels(std::vector<uint32> const& displayIds)
    {
        for (uint32 displayId : displayIds)
            loadGameObject(displayId);
    }

    bool MMapManager::loadGameObject(uint32 displayId)
    {
        // we already have this map loaded?
        if (m_loadedModels.find(displayId) != m_loadedModels.end())
            return true;

        // load and init dtNavMesh - read parameters from file
        uint32 pathLen = sWorld.GetDataPath().length() + strlen("mmaps/go%04i.mmtile") + 1;
        char* fileName = new char[pathLen];
        snprintf(fileName, pathLen, (sWorld.GetDataPath() + "mmaps/go%04i.mmtile").c_str(), displayId);

        FILE* file = fopen(fileName, "rb");
        if (!file)
        {
            DEBUG_LOG("MMAP:loadGameObject: Error: Could not open mmap file %s", fileName);
            delete[] fileName;
            return false;
        }

        MmapTileHeader fileHeader;
        fread(&fileHeader, sizeof(MmapTileHeader), 1, file);

        if (fileHeader.mmapMagic != MMAP_MAGIC)
        {
            sLog.outError("MMAP:loadGameObject: Bad header in mmap %s", fileName);
            fclose(file);
            return false;
        }

        if (fileHeader.mmapVersion != MMAP_VERSION)
        {
            sLog.outError("MMAP:loadGameObject: %s was built with generator v%i, expected v%i",
                fileName, fileHeader.mmapVersion, MMAP_VERSION);
            fclose(file);
            return false;
        }
        unsigned char* data = (unsigned char*)dtAlloc(fileHeader.size, DT_ALLOC_PERM);
        MANGOS_ASSERT(data);

        size_t result = fread(data, fileHeader.size, 1, file);
        if (!result)
        {
            sLog.outError("MMAP:loadGameObject: Bad header or data in mmap %s", fileName);
            fclose(file);
            return false;
        }

        fclose(file);

        dtNavMesh* mesh = dtAllocNavMesh();
        MANGOS_ASSERT(mesh);
        dtStatus r = mesh->init(data, fileHeader.size, DT_TILE_FREE_DATA);
        if (dtStatusFailed(r))
        {
            dtFreeNavMesh(mesh);
            sLog.outError("MMAP:loadGameObject: Failed to initialize dtNavMesh from file %s. Result 0x%x.", fileName, r);
            delete[] fileName;
            return false;
        }
        DETAIL_LOG("MMAP:loadGameObject: Loaded file %s [size=%u]", fileName, fileHeader.size);
        delete[] fileName;

        MMapGOData* mmap_data = new MMapGOData(mesh);
        m_loadedModels.insert(std::pair<uint32, MMapGOData*>(displayId, mmap_data));
        return true;
    }

    bool MMapManager::unloadMap(uint32 mapId, uint32 instanceId, int32 x, int32 y)
    {
        // check if we have this map loaded
        auto itr = m_loadedMMaps.find(packInstanceId(mapId, instanceId));
        if (itr == m_loadedMMaps.end())
        {
            // file may not exist, therefore not loaded
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Asked to unload not loaded navmesh map. %03u%02i%02i.mmtile", mapId, x, y);
            return false;
        }

        MMapData* mmap = (*itr).second;

        // check if we have this tile loaded
        uint32 packedGridPos = packTileID(x, y);
        if (mmap->mmapLoadedTiles.find(packedGridPos) == mmap->mmapLoadedTiles.end())
        {
            // file may not exist, therefore not loaded
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Asked to unload not loaded navmesh tile. %03u%02i%02i.mmtile", mapId, x, y);
            return false;
        }

        dtTileRef tileRef = mmap->mmapLoadedTiles[packedGridPos];

        // unload, and mark as non loaded
        dtStatus dtResult = mmap->navMesh->removeTile(tileRef, nullptr, nullptr);
        if (dtStatusFailed(dtResult))
        {
            // this is technically a memory leak
            // if the grid is later reloaded, dtNavMesh::addTile will return error but no extra memory is used
            // we cannot recover from this error - assert out
            sLog.outError("MMAP:unloadMap: Could not unload %03u%02i%02i.mmtile from navmesh", mapId, x, y);
            MANGOS_ASSERT(false);
        }
        else
        {
            mmap->mmapLoadedTiles.erase(packedGridPos);
            --m_loadedTiles;
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Unloaded mmtile %03i[%02i,%02i] from %03i", mapId, x, y, mapId);
            return true;
        }

        return false;
    }

    bool MMapManager::unloadMap(uint32 mapId)
    {
        bool success = false;
        // unload all maps with given mapId
        for (auto itr = m_loadedMMaps.begin(); itr != m_loadedMMaps.end();)
        {
            if (itr->first != uint64(mapId) << 32)
            {
                ++itr;
                continue;
            }

            // unload all tiles from given map
            MMapData* mmap = (*itr).second;
            for (MMapTileSet::iterator i = mmap->mmapLoadedTiles.begin(); i != mmap->mmapLoadedTiles.end(); ++i)
            {
                uint32 x = (i->first >> 16);
                uint32 y = (i->first & 0x0000FFFF);
                dtStatus dtResult = mmap->navMesh->removeTile(i->second, nullptr, nullptr);
                if (dtStatusFailed(dtResult))
                    sLog.outError("MMAP:unloadMap: Could not unload %03u%02i%02i.mmtile from navmesh", mapId, x, y);
                else
                {
                    --m_loadedTiles;
                    DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Unloaded mmtile %03i[%02i,%02i] from %03i", mapId, x, y, mapId);
                }
            }

            delete mmap;
            itr = m_loadedMMaps.erase(itr);
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Unloaded %03i.mmap", mapId);
            success = true;
        }

        if (!success)
            // file may not exist, therefore not loaded
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Asked to unload not loaded navmesh map %03u", mapId);

        return success;
    }

    bool MMapManager::unloadMapInstance(uint32 mapId, uint32 instanceId)
    {
        // check if we have this map loaded
        auto itr = m_loadedMMaps.find(packInstanceId(mapId, instanceId));
        if (itr == m_loadedMMaps.end())
        {
            // file may not exist, therefore not loaded
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMapInstance: Asked to unload not loaded navmesh map %03u", mapId);
            return false;
        }

        MMapData* mmap = (*itr).second;
        if (mmap->navMeshQueries.find(instanceId) == mmap->navMeshQueries.end())
        {
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMapInstance: Asked to unload not loaded dtNavMeshQuery mapId %03u instanceId %u", mapId, instanceId);
            return false;
        }

        dtNavMeshQuery* query = mmap->navMeshQueries[instanceId];

        dtFreeNavMeshQuery(query);
        mmap->navMeshQueries.erase(instanceId);
        DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMapInstance: Unloaded mapId %03u instanceId %u", mapId, instanceId);

        return true;
    }

    dtNavMesh const* MMapManager::GetNavMesh(uint32 mapId, uint32 instanceId)
    {
        auto itr = m_loadedMMaps.find(packInstanceId(mapId, instanceId));
        if (itr == m_loadedMMaps.end())
            return nullptr;

        return (*itr).second->navMesh;
    }

    dtNavMesh const* MMapManager::GetGONavMesh(uint32 mapId)
    {
        if (m_loadedModels.find(mapId) == m_loadedModels.end())
            return nullptr;

        return m_loadedModels[mapId]->navMesh;
    }

    dtNavMeshQuery const* MMapManager::GetNavMeshQuery(uint32 mapId, uint32 instanceId)
    {
        auto itr = m_loadedMMaps.find(packInstanceId(mapId, instanceId));
        if (itr == m_loadedMMaps.end())
            return nullptr;

        MMapData* mmap = (*itr).second;
        if (mmap->navMeshQueries.find(instanceId) == mmap->navMeshQueries.end())
        {
            // allocate mesh query
            dtNavMeshQuery* query = dtAllocNavMeshQuery();
            MANGOS_ASSERT(query);
            dtStatus dtResult = query->init(mmap->navMesh, 1024);
            if (dtStatusFailed(dtResult))
            {
                dtFreeNavMeshQuery(query);
                sLog.outError("MMAP:GetNavMeshQuery: Failed to initialize dtNavMeshQuery for mapId %03u instanceId %u", mapId, instanceId);
                return nullptr;
            }

            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:GetNavMeshQuery: created dtNavMeshQuery for mapId %03u instanceId %u", mapId, instanceId);
            mmap->navMeshQueries.insert(std::pair<uint32, dtNavMeshQuery*>(instanceId, query));
        }

        return mmap->navMeshQueries[instanceId];
    }

    dtNavMeshQuery const* MMapManager::GetModelNavMeshQuery(uint32 displayId)
    {
        if (m_loadedModels.find(displayId) == m_loadedModels.end())
            return nullptr;

        auto threadId = std::this_thread::get_id();
        MMapGOData* mmap = m_loadedModels[displayId];
        if (mmap->navMeshGOQueries.find(threadId) == mmap->navMeshGOQueries.end())
        {
            std::lock_guard<std::mutex> guard(m_modelsMutex);
            if (mmap->navMeshGOQueries.find(threadId) == mmap->navMeshGOQueries.end())
            {
                // allocate mesh query
                std::stringstream ss;
                ss << threadId;
                dtNavMeshQuery* query = dtAllocNavMeshQuery();
                MANGOS_ASSERT(query);
                if (dtStatusFailed(query->init(mmap->navMesh, 2048)))
                {
                    dtFreeNavMeshQuery(query);
                    sLog.outError("MMAP:GetNavMeshQuery: Failed to initialize dtNavMeshQuery for displayid %03u tid %s", displayId, ss.str().data());
                    return nullptr;
                }

                DETAIL_LOG("MMAP:GetNavMeshQuery: created dtNavMeshQuery for displayid %03u tid %s", displayId, ss.str().data());
                mmap->navMeshGOQueries.insert(std::pair<std::thread::id, dtNavMeshQuery*>(threadId, query));
            }
        }

        return mmap->navMeshGOQueries[threadId];
    }
}
