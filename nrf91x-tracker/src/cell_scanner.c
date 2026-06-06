#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>
#include <string.h>

#include "tracker.h"

LOG_MODULE_REGISTER(cell_scanner, LOG_LEVEL_INF);

/* ---- Internal state ---------------------------------------------------- */

static struct k_sem meas_done_sem;
static struct cell_scan_data *result_ptr;

/*
 * 3GPP TS 36.133 conversions:
 *   RSRP(dBm) = raw_value - 140   (range: -140 to -43 dBm)
 *   RSRQ(dB)  = raw_value/2 - 19  (range: -19.5 to -3 dB, reported *2)
 */
static inline int16_t rsrp_to_dbm(int16_t raw) { return raw - 140; }
static inline int16_t rsrq_to_db(int16_t raw)  { return raw / 2 - 19; }

/* ---- lte_lc event handler (registered alongside main.c's handler) ------ */

static void cell_lte_handler(const struct lte_lc_evt *const evt)
{
    if (evt->type != LTE_LC_EVT_NEIGHBOR_CELL_MEAS) {
        return;
    }

    struct cell_scan_data *res = result_ptr;
    if (!res) {
        return;
    }

    const struct lte_lc_cells_info *info = &evt->cells_info;

    /* Serving cell */
    const struct lte_lc_cell *s = &info->current_cell;
    if (s->id != LTE_LC_CELL_EUTRAN_ID_INVALID) {
        res->serving.mcc             = s->mcc;
        res->serving.mnc             = s->mnc;
        res->serving.tac             = s->tac;
        res->serving.cell_id         = s->id;
        res->serving.earfcn          = s->earfcn;
        res->serving.pci             = s->phys_cell_id;
        res->serving.rsrp_dbm        = rsrp_to_dbm(s->rsrp);
        res->serving.rsrq_db         = rsrq_to_db(s->rsrq);
        res->serving.timing_advance  = s->timing_advance;
        res->serving.valid           = true;
    }

    /* Neighbor cells */
    res->neighbor_count = MIN(info->ncells_count, CELL_MAX_NEIGHBORS);
    for (int i = 0; i < res->neighbor_count; i++) {
        const struct lte_lc_ncell *n = &info->neighbor_cells[i];
        res->neighbors[i].earfcn   = n->earfcn;
        res->neighbors[i].pci      = n->phys_cell_id;
        res->neighbors[i].rsrp_dbm = rsrp_to_dbm(n->rsrp);
        res->neighbors[i].rsrq_db  = rsrq_to_db(n->rsrq);
        res->neighbors[i].time_diff = n->time_diff;
    }

    LOG_INF("Cell meas: serving cell_id=%u MCC=%u MNC=%u RSRP=%ddBm, "
            "%d neighbor(s)",
            res->serving.cell_id, res->serving.mcc, res->serving.mnc,
            res->serving.rsrp_dbm, res->neighbor_count);

    k_sem_give(&meas_done_sem);
}

/* ---- Public API -------------------------------------------------------- */

int cell_scanner_init(void)
{
    k_sem_init(&meas_done_sem, 0, 1);
    lte_lc_register_handler(cell_lte_handler);
    LOG_INF("Cell scanner ready");
    return 0;
}

int cell_scanner_measure(struct cell_scan_data *out, k_timeout_t timeout)
{
    int err;

    memset(out, 0, sizeof(*out));
    result_ptr = out;
    k_sem_reset(&meas_done_sem);

    struct lte_lc_ncellmeas_params params = {
        .search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_DEFAULT,
        .gci_count   = 0,
    };

    err = lte_lc_neighbor_cell_measurement(&params);
    if (err) {
        LOG_ERR("Neighbor cell measurement request failed (%d)", err);
        result_ptr = NULL;
        return err;
    }

    err = k_sem_take(&meas_done_sem, timeout);
    result_ptr = NULL;

    if (err == -EAGAIN) {
        LOG_WRN("Cell measurement timeout");
        lte_lc_neighbor_cell_measurement_cancel();
        return -ETIMEDOUT;
    }

    return 0;
}
