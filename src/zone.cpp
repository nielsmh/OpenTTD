/*
* This file is part of OpenTTD.
* OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
* OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file zone.cpp Handling and validation of map zones. */

#include "stdafx.h"
#include "zone_type.h"
#include "zone_func.h"
#include "town.h"
#include "map_type.h"


MapZone _map_zones[0x100];

static void ResetZone(MapZone &zone)
{
	for (MapZoneBuildRestriction &r : zone.company_restrictions) {
		r = MZR_NONE;
	}
	zone.main_town = INVALID_TOWN;
	zone.industry_restriction = MZIR_NONE;
	zone.in_use = false;
	free(zone.name);
	zone.name = NULL;
}

static void ResetDefaultZone()
{
	ResetZone(_map_zones[MZ_DEFAULT]);
	_map_zones[MZ_DEFAULT].in_use = true;
}

static void ResetOceanZone(bool use)
{
	MapZone &zone = _map_zones[MZ_OCEAN];
	ResetZone(zone);

	zone.in_use = use;
	zone.company_restrictions[OWNER_DEITY] = MZR_TERRAFORM | MZR_BRIDGE_ABOVE | MZR_TUNNEL_BELOW | MZR_CLEAR_WATER;
}

static void ResetMountainZone(bool use)
{
	MapZone &zone = _map_zones[MZ_OCEAN];
	ResetZone(zone);

	zone.in_use = use;
	zone.company_restrictions[OWNER_DEITY] = MZR_TERRAFORM;
}

void ResetMapZones()
{
	for (MapZone &zone : _map_zones) ResetZone(zone);

	ResetDefaultZone();
	ResetOceanZone(false);
	ResetMountainZone(false);
}

/**
 * Retrieve the map zone object for a tile.
 */
const MapZone *MapZone::GetForTile(TileIndex tile)
{
	assert(IsValidTile(tile));
	MapZone *zone = &_map_zones[_me[tile].zone];
	assert(zone->in_use);
	return zone;
}

/**
 * Check if a zone allows a particular type of construction.
 * @param company  Company wanting to construct, or OWNER_TOWN.
 * @param flags    Construction types to check for.
 * @return True if none of the \c flags are restricted.
 */
bool MapZone::AllowsConstruction(CompanyID company, MapZoneBuildRestriction flags)
{
	assert(this->in_use);
	assert(company < OWNER_END);

	MapZoneBuildRestriction total = this->company_restrictions[company] | this->company_restrictions[OWNER_DEITY];

	return (~total & flags) == flags;
}

