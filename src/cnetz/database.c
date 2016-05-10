/* C-Netz database
 *
 * (C) 2016 by Andreas Eversberg <jolly@eversberg.eu>
 * All Rights Reserved
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../common/debug.h"
#include "../common/timer.h"
#include "cnetz.h"
#include "database.h"

#define MELDE_INTERVAL		60.0
#define MELDE_WIEDERHOLUNG	10.0
#define MELDE_MAXIMAL		3

typedef struct cnetz_database {
	struct cnetz_database	*next;
	uint8_t			futln_nat;	/* who ... */
	uint8_t			futln_fuvst;
	uint16_t		futln_rest;
	struct timer		timer;		/* timer for next availability check */
	int			retry;		/* counts number of retries */
} cnetz_db_t;

cnetz_db_t *cnetz_db_head;

/* destroy transaction */
static void remove_db(cnetz_db_t *db)
{
	cnetz_db_t **dbp;

	/* uinlink */
	dbp = &cnetz_db_head;
	while (*dbp && *dbp != db)
		dbp = &((*dbp)->next);
	if (!(*dbp)) {
		PDEBUG(DDB, DEBUG_ERROR, "Transaction not in list, please fix!!\n");
		abort();
	}
	*dbp = db->next;

	PDEBUG(DDB, DEBUG_INFO, "Removing subscriber '%d,%d,%d' from database.\n", db->futln_nat, db->futln_fuvst, db->futln_rest);

	timer_exit(&db->timer);

	free(db);
}

/* Timeout handling */
static void db_timeout(struct timer *timer)
{
	cnetz_db_t *db = (cnetz_db_t *)timer->priv;
	int rc;

	PDEBUG(DDB, DEBUG_INFO, "Check, if subscriber '%d,%d,%d' is still available.\n", db->futln_nat, db->futln_fuvst, db->futln_rest);
	
	rc = cnetz_meldeaufruf(db->futln_nat, db->futln_fuvst, db->futln_rest);
	if (rc < 0) {
		/* OgK is used for speech, but this never happens in a real
		 * network. We just assume that the phone has responded and
		 * assume we had a response. */
		PDEBUG(DDB, DEBUG_INFO, "OgK busy, so we assume a positive response.\n");
		timer_start(&db->timer, MELDE_INTERVAL); /* when to check avaiability again */
		db->retry = 0;
	}
}

/* create/update db entry */
void update_db(cnetz_t *cnetz, uint8_t futln_nat, uint8_t futln_fuvst, uint16_t futln_rest, int busy, int failed)
{
	cnetz_db_t *db, **dbp;

	/* search transaction for this subsriber */
	db = cnetz_db_head;
	while (db) {
		if (db->futln_nat == futln_nat
		 && db->futln_fuvst == futln_fuvst
		 && db->futln_rest == futln_rest)
			break;
		db = db->next;
	}
	if (!db) {
		db = calloc(1, sizeof(*db));
		if (!db) {
			PDEBUG(DDB, DEBUG_ERROR, "No memory!\n");
			return;
		}
		timer_init(&db->timer, db_timeout, db);

		db->futln_nat = futln_nat;
		db->futln_fuvst = futln_fuvst;
		db->futln_rest = futln_rest;

		/* attach to end of list */
		dbp = &cnetz_db_head;
		while (*dbp)
			dbp = &((*dbp)->next);
		*dbp = db;

		PDEBUG(DDB, DEBUG_INFO, "Adding subscriber '%d,%d,%d' to database.\n", db->futln_nat, db->futln_fuvst, db->futln_rest);
	}

	if (busy) {
		PDEBUG(DDB, DEBUG_INFO, "Subscriber '%d,%d,%d' busy now.\n", db->futln_nat, db->futln_fuvst, db->futln_rest);
		timer_stop(&db->timer);
	} else if (!failed) {
		PDEBUG(DDB, DEBUG_INFO, "Subscriber '%d,%d,%d' idle now.\n", db->futln_nat, db->futln_fuvst, db->futln_rest);
		timer_start(&db->timer, MELDE_INTERVAL); /* when to check avaiability (again) */
		db->retry = 0;
	} else {
		if (++db->retry == MELDE_MAXIMAL) {
			PDEBUG(DDB, DEBUG_NOTICE, "Paging subscriber '%d,%d,%d' failed.\n", db->futln_nat, db->futln_fuvst, db->futln_rest);
			remove_db(db);
			return;
		}
		timer_start(&db->timer, MELDE_WIEDERHOLUNG); /* when to do retry */
	}
}

int find_db(uint8_t futln_nat, uint8_t futln_fuvst, uint16_t futln_rest)
{
	cnetz_db_t *db = cnetz_db_head;

	while (db) {
		if (db->futln_nat == futln_nat
		 && db->futln_fuvst == futln_fuvst
		 && db->futln_rest == futln_rest)
			return 1;
		db = db->next;
	}
	return 0;
}

void flush_db(void)
{
	while (cnetz_db_head)
		remove_db(cnetz_db_head);
}
