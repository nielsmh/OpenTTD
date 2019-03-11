/*
* This file is part of OpenTTD.
* OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
* OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file zone_func.h Functions related to map zones. */

#ifndef ZONE_TYPE_H
#define ZONE_TYPE_H

#include "stdafx.h"
#include "company_type.h"
#include "town_type.h"
#include "tile_type.h"

/**
 * Predefined map zones.
 */
enum MapTileZone : byte {
	MZ_DEFAULT = 0,  ///< Default map zone with no particular rules
	MZ_OCEAN,        ///< Deep ocean map zone
	MZ_SEA,          ///< Shallow ocean map zone
	MZ_MOUNTAIN,     ///< Bedrock mountain zone
	/* Values up to MZ_USER1 are reserved for future built-in zones */
	MZ_USER1 = 0x10, ///< First scenario-defined zone, all values past this are free
	MZ_LAST = 0xFF,  ///< Last valid zone (inclusive)
};

/**
 * Types of constructtion disallowed in a map zone.
 */
enum MapZoneBuildRestriction : uint32 {
	MZR_NONE = 0,
	MZR_TERRAFORM      = 1 << 0,  ///< May not change tile heights
	MZR_CLEAR_NATURE   = 1 << 1,  ///< May not clear natural features (trees, rocky patches)
	MZR_CLEAR_WATER    = 1 << 2,  ///< May not clear water tiles (incl. river tiles, also block convert to canals)
	MZR_CLEAR_OBJECT   = 1 << 3,  ///< May not clear other map objects
	MZR_BRIDGE_ABOVE   = 1 << 4,  ///< May not bridge above the zone
	MZR_BRIDGE_HEAD    = 1 << 5,  ///< May not start/end bridges in the zone
	MZR_TUNNEL_BELOW   = 1 << 6,  ///< May not tunnel below the zone
	MZR_TUNNEL_HEAD    = 1 << 7,  ///< May not start/end tunnels in the zone
	MZR_ROAD           = 1 << 8,  ///< May not build roads/trams
	MZR_RAIL           = 1 << 9,  ///< May not build railroads
	MZR_RAIL_FAST      = 1 << 10, ///< May not build high-speed rail (incl. monorail/maglev)
	MZR_CANALS         = 1 << 11, ///< May not build canals or ship locks in the zone (nor convert rivers)
	MZR_AIRPORT_SM     = 1 << 12, ///< May not build small airports
	MZR_AIRPORT_LG     = 1 << 13, ///< May not build large airports
	MZR_AIRPORT_HU     = 1 << 14, ///< May not build huge (international+) airports
	MZR_HELIPORT       = 1 << 15, ///< May not build heliports, helidepots, etc.
	MZR_HEADQUARTER    = 1 << 16, ///< May not build company HQ
	MZR_BUY_LAND       = 1 << 17, ///< May not buy land in zone
	MZR_FOUND_INDUSTRY = 1 << 18, ///< May not found industries in zone (and prospecting auto-fails in this zone)
	MZR_FOUND_TOWN     = 1 << 19, ///< May not found towns in this zone
	MZR_STATION_BUS    = 1 << 20, ///< May not build bus stations/stops
	MZR_STATION_TRUCK  = 1 << 21, ///< May not build truck loading bays/stops
	MZR_STATION_RAIL   = 1 << 22, ///< May not build rail stations
	MZR_STATION_DOCK   = 1 << 23, ///< May not build docks
	MZR_ALL = 0xFFFFFFFF, ///< May not do anything at all
};
DECLARE_ENUM_AS_BIT_SET(MapZoneBuildRestriction)

enum MapZoneIndustryRestriction : byte {
	MZIR_NONE = 0,
	MZIR_BLACK_HOLE = 1 << 0,
	MZIR_EXTRACTIVE = 1 << 1,
	MZIR_ORGANIC    = 1 << 2,
	MZIR_PROCESSING = 1 << 3,
};
DECLARE_ENUM_AS_BIT_SET(MapZoneIndustryRestriction)

/**
 * Definition of a map zone's rules.
 */
struct MapZone {
	/**
	 * Individual restrictions for each company in the zone.
	 * Index \c OWNER_TOWN applies to towns wanting to expand.
	 * Index \c OWNER_DEITY applies to everyone (including towns).
	 * Indexes \c OWNER_NONE and \c OWNER_WATER have no defined meaning.
	 */
	MapZoneBuildRestriction company_restrictions[OWNER_END];

	/**
	 * The main/capital town in the zone.
	 * If none, use \c INVALID_TOWN.
	 * Must be \c INVALID_TOWN for built-in zones (below \c MZ_USER1).
	 */
	TownID main_town;

	/** General restrictions on industry types that may be built */
	MapZoneIndustryRestriction industry_restriction;

	/** Is this zone allocated for use? */
	bool in_use;

	/**
	 * Scenario-assigned name.
	 * If this is \c NULL a default name is used:
	 * - For built-in zones, a built-in name is used.
	 * - For zones with a \c main_town set, a name based on the town name is used.
	 * - Otherwise, a name based on the zone number is used.
	 */
	char *name;

	static const MapZone *GetForTile(TileIndex tile);

	bool AllowsConstruction(CompanyID company, MapZoneBuildRestriction flags);
};

extern MapZone _map_zones[0x100];

#endif
