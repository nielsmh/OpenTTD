/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file depot_cmd.cpp %Command Handling for depots. */

#include "stdafx.h"
#include "command_func.h"
#include "depot_base.h"
#include "company_func.h"
#include "date_func.h"
#include "string_func.h"
#include "strings_func.h"
#include "landscape.h"
#include "town.h"
#include "vehicle_gui.h"
#include "vehiclelist.h"
#include "window_func.h"
#include "viewport_kdtree.h"

#include "table/strings.h"

#include "safeguards.h"

/**
 * Check whether the given name is globally unique amongst depots.
 * @param name The name to check.
 * @return True if there is no depot with the given name.
 */
static bool IsUniqueDepotName(const char *name)
{
	const Depot *d;

	FOR_ALL_DEPOTS(d) {
		if (d->name != NULL && strcmp(d->name, name) == 0) return false;
	}

	return true;
}

/**
 * Find a demolished depot close to a tile.
 * @param tile Tile to search from.
 * @param type Depot type.
 * @param cid Previous owner of the depot.
 * @return The demolished nearby depot.
 */
Depot *FindDeletedDepotCloseTo(TileIndex tile, VehicleType type, CompanyID cid)
{
	Depot *depot, *best_depot = NULL;
	uint best_dist = 8;

	FOR_ALL_DEPOTS(depot) {
		if (!depot->IsInUse() && depot->type == type && depot->owner == cid) {
			uint cur_dist = DistanceManhattan(tile, depot->xy);

			if (cur_dist < best_dist) {
				best_dist = cur_dist;
				best_depot = depot;
			}
		}
	}

	return best_depot;
}

/**
 * Rename a depot.
 * @param tile unused
 * @param flags type of operation
 * @param p1 id of depot
 * @param p2 unused
 * @param text the new name or an empty string when resetting to the default
 * @return the cost of this operation or an error
 */
CommandCost CmdRenameDepot(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	Depot *d = Depot::GetIfValid(p1);
	if (d == NULL || !d->IsInUse()) return CMD_ERROR;

	CommandCost ret = CheckOwnership(d->owner);
	if (ret.Failed()) return ret;

	bool reset = StrEmpty(text);

	if (!reset) {
		if (Utf8StringLength(text) >= MAX_LENGTH_DEPOT_NAME_CHARS) return CMD_ERROR;
		if (!IsUniqueDepotName(text)) return_cmd_error(STR_ERROR_NAME_MUST_BE_UNIQUE);
	}

	if (flags & DC_EXEC) {
		/* _viewport_sign_kdtree does not need to be updated, only in-use depots can be renamed */

		free(d->name);

		if (reset) {
			d->name = NULL;
			MakeDefaultName(d);
		} else {
			d->name = stredup(text);
		}

		/* Update the orders and depot */
		SetWindowClassesDirty(WC_VEHICLE_ORDERS);
		SetWindowDirty(WC_VEHICLE_DEPOT, d->xy);

		/* Update the depot list */
		SetWindowDirty(GetWindowClassForVehicleType(d->type), VehicleListIdentifier(VL_DEPOT_LIST, d->type, d->owner, d->index).Pack());
	}
	return CommandCost();
}

/** Update the virtual coords needed to draw the depot sign. */
void Depot::UpdateVirtCoord()
{
	Point pt = RemapCoords2(TileX(this->xy) * TILE_SIZE, TileY(this->xy) * TILE_SIZE);

	pt.y -= 32 * ZOOM_LVL_BASE;

	SetDParam(0, this->type);
	SetDParam(1, this->index);
	this->sign.UpdatePosition(pt.x, pt.y, STR_VIEWPORT_DEPOT, STR_VIEWPORT_DEPOT_TINY);

	SetWindowDirty(WC_VEHICLE_DEPOT, this->index);
}

/** Update the virtual coords needed to draw the depot sign for all depots. */
void UpdateAllDepotVirtCoords()
{
	/* Only demolished depots have signs. */
	Depot *d;
	FOR_ALL_DEPOTS(d) if (!d->IsInUse()) d->UpdateVirtCoord();
}

void OnTick_Depot()
{
	if (_game_mode == GM_EDITOR) return;

	/* Clean up demolished depots. */
	Depot *d;
	FOR_ALL_DEPOTS(d) {
		if ((_tick_counter + d->index) % DEPOT_REMOVAL_TICKS == 0) {
			if (!d->IsInUse() && --d->delete_ctr == 0) {
				_viewport_sign_kdtree.Remove(ViewportSignKdtreeItem::MakeDepot(d->index));
				delete d;
			}
		}
	}
}
