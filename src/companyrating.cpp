/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

 /** @file companyrating.cpp Handling of company performance ratings. */

#include "stdafx.h"
#include "company_base.h"
#include "company_func.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "station_base.h"
#include "object.h"
#include "window_func.h"


/**
 * Score info, values used for computing the detailed performance rating.
 */
const ScoreInfo _score_info[] = {
	{     120, 100, SCOREUNIT_COUNT, STR_PERFORMANCE_DETAIL_VEHICLES,   STR_PERFORMANCE_DETAIL_VEHICLES_TOOLTIP  }, // SCORE_VEHICLES
	{      80, 100, SCOREUNIT_COUNT, STR_PERFORMANCE_DETAIL_STATIONS,   STR_PERFORMANCE_DETAIL_STATIONS_TOOLTIP  }, // SCORE_STATIONS
	{   10000, 100, SCOREUNIT_MONEY, STR_PERFORMANCE_DETAIL_MIN_PROFIT, STR_PERFORMANCE_DETAIL_MIN_PROFIT_TOOLTIP}, // SCORE_MIN_PROFIT
	{   50000,  50, SCOREUNIT_MONEY, STR_PERFORMANCE_DETAIL_MIN_INCOME, STR_PERFORMANCE_DETAIL_MIN_INCOME_TOOLTIP}, // SCORE_MIN_INCOME
	{  100000, 100, SCOREUNIT_MONEY, STR_PERFORMANCE_DETAIL_MAX_INCOME, STR_PERFORMANCE_DETAIL_MAX_INCOME_TOOLTIP}, // SCORE_MAX_INCOME
	{   40000, 400, SCOREUNIT_COUNT, STR_PERFORMANCE_DETAIL_DELIVERED,  STR_PERFORMANCE_DETAIL_DELIVERED_TOOLTIP }, // SCORE_DELIVERED
	{       8,  50, SCOREUNIT_COUNT, STR_PERFORMANCE_DETAIL_CARGO,      STR_PERFORMANCE_DETAIL_CARGO_TOOLTIP     }, // SCORE_CARGO
	{10000000,  50, SCOREUNIT_MONEY, STR_PERFORMANCE_DETAIL_MONEY,      STR_PERFORMANCE_DETAIL_MONEY_TOOLTIP     }, // SCORE_MONEY
	{  250000,  50, SCOREUNIT_MONEY, STR_PERFORMANCE_DETAIL_LOAN,       STR_PERFORMANCE_DETAIL_LOAN_TOOLTIP      }, // SCORE_LOAN
	{       0,   0, SCOREUNIT_COUNT, STR_PERFORMANCE_DETAIL_TOTAL,      STR_PERFORMANCE_DETAIL_TOTAL_TOOLTIP     }  // SCORE_TOTAL
};

int64 _score_part[MAX_COMPANIES][SCORE_END];

/**
 * if update is set to true, the economy is updated with this score
 *  (also the house is updated, should only be true in the on-tick event)
 * @param update the economy with calculated score
 * @param c company been evaluated
 * @return actual score of this company
 *
 */
int UpdateCompanyRatingAndValue(Company *c, bool update)
{
	Owner owner = c->index;
	int score = 0;

	memset(_score_part[owner], 0, sizeof(_score_part[owner]));

	/* Count vehicles */
	{
		Money min_profit = 0;
		bool min_profit_first = true;
		uint num = 0;

		for (const Vehicle *v : Vehicle::Iterate()) {
			if (v->owner != owner) continue;
			if (IsCompanyBuildableVehicleType(v->type) && v->IsPrimaryVehicle()) {
				if (v->profit_last_year > 0) num++; // For the vehicle score only count profitable vehicles
				if (v->age > 730) {
					/* Find the vehicle with the lowest amount of profit */
					if (min_profit_first || min_profit > v->profit_last_year) {
						min_profit = v->profit_last_year;
						min_profit_first = false;
					}
				}
			}
		}

		min_profit >>= 8; // remove the fract part

		_score_part[owner][SCORE_VEHICLES] = num;
		/* Don't allow negative min_profit to show */
		if (min_profit > 0) {
			_score_part[owner][SCORE_MIN_PROFIT] = min_profit;
		}
	}

	/* Count stations */
	{
		uint num = 0;
		for (const Station *st : Station::Iterate()) {
			/* Only count stations that are actually serviced */
			if (st->owner == owner && (st->time_since_load <= 20 || st->time_since_unload <= 20)) num += CountBits((byte)st->facilities);
		}
		_score_part[owner][SCORE_STATIONS] = num;
	}

	/* Generate statistics depending on recent income statistics */
	{
		int numec = min(c->num_valid_stat_ent, 12);
		if (numec != 0) {
			const CompanyEconomyEntry *cee = c->old_economy;
			Money min_income = cee->income + cee->expenses;
			Money max_income = cee->income + cee->expenses;

			do {
				min_income = min(min_income, cee->income + cee->expenses);
				max_income = max(max_income, cee->income + cee->expenses);
			} while (++cee, --numec);

			if (min_income > 0) {
				_score_part[owner][SCORE_MIN_INCOME] = min_income;
			}

			_score_part[owner][SCORE_MAX_INCOME] = max_income;
		}
	}

	/* Generate score depending on amount of transported cargo */
	{
		int numec = min(c->num_valid_stat_ent, 4);
		if (numec != 0) {
			const CompanyEconomyEntry *cee = c->old_economy;
			OverflowSafeInt64 total_delivered = 0;
			do {
				total_delivered += cee->delivered_cargo.GetSum<OverflowSafeInt64>();
			} while (++cee, --numec);

			_score_part[owner][SCORE_DELIVERED] = total_delivered;
		}
	}

	/* Generate score for variety of cargo */
	{
		_score_part[owner][SCORE_CARGO] = c->old_economy->delivered_cargo.GetCount();
	}

	/* Generate score for company's money */
	{
		if (c->money > 0) {
			_score_part[owner][SCORE_MONEY] = c->money;
		}
	}

	/* Generate score for loan */
	{
		_score_part[owner][SCORE_LOAN] = _score_info[SCORE_LOAN].needed - c->current_loan;
	}

	/* Now we calculate the score for each item.. */
	{
		int total_score = 0;
		int s;
		score = 0;
		for (ScoreID i = SCORE_BEGIN; i < SCORE_END; i++) {
			/* Skip the total */
			if (i == SCORE_TOTAL) continue;
			/*  Check the score */
			s = Clamp<int64>(_score_part[owner][i], 0, _score_info[i].needed) * _score_info[i].score / _score_info[i].needed;
			score += s;
			total_score += _score_info[i].score;
		}

		_score_part[owner][SCORE_TOTAL] = score;

		/*  We always want the score scaled to SCORE_MAX (1000) */
		if (total_score != SCORE_MAX) score = score * SCORE_MAX / total_score;
	}

	if (update) {
		c->old_economy[0].performance_history = score;
		UpdateCompanyHQ(c->location_of_HQ, score);
		c->old_economy[0].company_value = CalculateCompanyValue(c);
	}

	SetWindowDirty(WC_PERFORMANCE_DETAIL, 0);
	return score;
}
