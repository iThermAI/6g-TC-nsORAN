# ad-slicing.py  -- Hierarchical assigner (split files) + per-cluster VAE + Influx stream
# ASCII-only prints for safe terminals.

import torch
import joblib
import numpy as np
import pandas as pd
import torch.nn as nn
import os, time, logging, traceback
from pathlib import Path
from typing import Tuple, Dict, Any
from influxdb import InfluxDBClient

# =========================
# Config
# --- Slice/Cluster ↔ CellID mapping ---
CLUSTER_TO_CELLID = {0: 2, 1: 3, 2: 4}
CLUSTER_NAME = {0: "eMBB", 1: "uRLLC", 2: "mMTC"}
CELLID_NAME = {2: "eMBB", 3: "uRLLC", 4: "mMTC"}

def _fmt_num(val):
	return f"{val:.2f}" if isinstance(val, (int, float)) else "N/A"

# =========================
QUERY_INTERVAL = 0.001  # seconds
MODELS_DIR = Path("models_dir")  # folder containing hier_*.pkl and VAE files
DEBUG = True  # set False to reduce logs

# =========================
# Logging
# =========================
logging.basicConfig(
	filename='anomalies.log',
	level=logging.WARNING,
	format='%(asctime)s - %(message)s'
)

# =========================
# VAE (as provided)
# =========================
class VAE(nn.Module):
	def __init__(self, input_dim, latent_dim=4):
		super(VAE, self).__init__()
		self.encoder = nn.Sequential(
			nn.Linear(input_dim, 32), nn.ReLU(),
			nn.Linear(32, 16), nn.ReLU()
		)
		self.fc_mu = nn.Linear(16, latent_dim)
		self.fc_logvar = nn.Linear(16, latent_dim)
		self.decoder = nn.Sequential(
			nn.Linear(latent_dim, 16), nn.ReLU(),
			nn.Linear(16, 32), nn.ReLU(),
			nn.Linear(32, input_dim)
		)

	def reparameterize(self, mu, logvar):
		std = torch.exp(0.5 * logvar)
		eps = torch.randn_like(std)
		return mu + eps * std

	def forward(self, x):
		h = self.encoder(x)
		mu = self.fc_mu(h)
		logvar = self.fc_logvar(h)
		z = self.reparameterize(mu, logvar)
		recon = self.decoder(z)
		return recon, mu, logvar

# =========================
# Assigner loader from split files
# =========================
NEEDED_KEYS = {"selected_columns", "scaler", "centroids", "invs", "distance_metric"}

def load_assigner_split(models_dir: Path) -> dict:
	"""Load hierarchical assigner parts from separate files under models_dir."""
	models_dir = Path(models_dir)

	sel_path  = models_dir / "hier_selected_columns.pkl"
	sc_path   = models_dir / "hier_scaler.pkl"
	cen_path  = models_dir / "hier_centroids.pkl"
	inv_path  = models_dir / "hier_invs.pkl"
	dm_path   = models_dir / "hier_distance_metric.pkl"
	cov_path  = models_dir / "hier_covs.pkl"          # optional
	mdl_path  = models_dir / "hier_model.pkl"         # optional

	if not sel_path.exists() or not sc_path.exists() or not cen_path.exists() or not inv_path.exists() or not dm_path.exists():
		raise FileNotFoundError("Missing one or more hier_* files. Expected: hier_selected_columns.pkl, hier_scaler.pkl, hier_centroids.pkl, hier_invs.pkl, hier_distance_metric.pkl")

	selected_columns = joblib.load(sel_path)
	scaler           = joblib.load(sc_path)
	centroids        = joblib.load(cen_path)
	invs             = joblib.load(inv_path)
	distance_metric  = joblib.load(dm_path)

	# normalize types
	selected_columns = list(selected_columns)
	centroids = np.asarray(centroids)
	invs = [np.asarray(m) for m in invs]
	distance_metric = str(distance_metric).lower().strip()

	# optional loads (not required by inference)
	covs  = joblib.load(cov_path) if cov_path.exists() else None
	_mdl  = joblib.load(mdl_path) if mdl_path.exists() else None

	assigner = {
		"selected_columns": selected_columns,
		"scaler": scaler,
		"centroids": centroids,
		"invs": invs,
		"distance_metric": distance_metric,
		"covs": covs,         # unused unless you need it
		"agg_model": _mdl,    # unused by this script
	}

	# sanity checks
	if distance_metric not in ("euclidean", "mahalanobis"):
		print("[ASSIGNER][warn] distance_metric is '{}', defaulting to 'euclidean'".format(distance_metric))
		assigner["distance_metric"] = "euclidean"

	K = centroids.shape[0]
	if assigner["distance_metric"] == "mahalanobis" and (invs is None or len(invs) != K):
		raise ValueError("For mahalanobis, hier_invs.pkl must contain K inverse cov matrices.")

	return assigner

# =========================
# Utils
# =========================
def _align_columns_for_scaler(df: pd.DataFrame, cols_expected: list, scaler) -> pd.DataFrame:
	"""Align to scaler/expected order, create missing cols, coerce numeric, fill NaN median."""
	if hasattr(scaler, "feature_names_in_"):
		expected = list(scaler.feature_names_in_)
	else:
		expected = list(cols_expected)

	for c in expected:
		if c not in df.columns:
			df[c] = np.nan

	X = df[expected].copy()
	X.replace([np.inf, -np.inf], np.nan, inplace=True)
	for c in expected:
		if not np.issubdtype(X[c].dtype, np.number):
			X[c] = pd.to_numeric(X[c], errors="coerce")
	X.fillna(X.median(numeric_only=True), inplace=True)
	return X

def load_cluster_vaes(models_dir: Path, K: int) -> Dict[int, Dict[str, Any]]:
	"""Load (vae_model_class{k}.pth + vae_bundle_class{k}.pkl) per cluster."""
	vaes = {}
	for k in range(K):
		pth = models_dir / f"vae_model_class{k}.pth"
		pkl = models_dir / f"vae_bundle_class{k}.pkl"
		if not pth.exists() or not pkl.exists():
			logging.warning("[INIT] Missing VAE files for cluster {}: {} / {}".format(k, pth.name, pkl.name))
			continue
		bundle = joblib.load(pkl)
		feats, scaler, thr = bundle["features"], bundle["scaler"], float(bundle["threshold"])
		vae = VAE(input_dim=len(feats))
		vae.load_state_dict(torch.load(pth, map_location="cpu"))
		vae.eval()
		vaes[k] = {"vae": vae, "features": feats, "scaler": scaler, "threshold": thr}
	if not vaes:
		raise RuntimeError("No per-cluster VAE models found.")
	return vaes

# =========================
# Cluster assignment for one row
# =========================
def assign_cluster_for_row(row_dict: Dict[str, Any], assigner: Dict[str, Any]) -> int:
	cols = assigner["selected_columns"]
	scaler = assigner["scaler"]
	C = assigner["centroids"]
	invs = assigner["invs"]
	metric = assigner["distance_metric"]

	df_row = pd.DataFrame([row_dict])
	Xn = _align_columns_for_scaler(df_row, cols_expected=cols, scaler=scaler)
	Xs = scaler.transform(Xn)  # (1, D)

	if metric == "euclidean":
		dists = np.linalg.norm(Xs - C, axis=1)
		return int(np.argmin(dists))

	elif metric == "mahalanobis":
		K = C.shape[0]
		d = np.zeros(K)
		for k in range(K):
			diff = Xs[0] - C[k]
			d[k] = float(diff @ invs[k] @ diff)
		return int(np.argmin(d))

	else:
		raise ValueError("distance_metric must be 'euclidean' or 'mahalanobis'.")

# =========================
# VAE inference for one row
# =========================
def vae_anomaly_for_row(row_dict: Dict[str, Any], vae_pack: Dict[str, Any]) -> Tuple[float, int]:
	vae = vae_pack["vae"]
	feats = vae_pack["features"]
	scaler = vae_pack["scaler"]
	thr = vae_pack["threshold"]

	df_row = pd.DataFrame([row_dict])
	X = _align_columns_for_scaler(df_row, cols_expected=feats, scaler=scaler)
	Xs = scaler.transform(X)
	x = torch.as_tensor(Xs, dtype=torch.float32)

	with torch.no_grad():
		recon, _, _ = vae(x)
		rec_err = float(((recon - x)**2).mean().item())
	y_hat = int(rec_err > thr)
	return rec_err, y_hat

# =========================
# InfluxDB streaming (3 buckets)
# =========================
def query_influxdb():
	"""
	Streams joined rows from:
	  - cu_cp_bucket
	  - du_bucket: "drb_uethp_dl_ueid", rru_prb_used_dl, tb_err_total_nbr_dl_1
	  - cu_up_bucket: "drb_pdcp_sdu_volume_dl_filter_ueid_tx_bytes",
					  "tot_pdcp_sdu_nbr_dl_ueid_tx_dl_packets",
					  "drb_pdcp_sdu_delay_dl_ueid_pdcp_latency"
	Join key: (ue_imsi_complete, time)
	"""
	client = InfluxDBClient(host='influxdb', port=8086, database='ns3_metrics')
	last_time_seen = "1970-01-01T00:00:00.000000Z"

	while True:
		cu_query = f"""
			SELECT
				num_active_ues,
				l3_serving_sinr,
				l3_serving_sinr_3gpp,
				ue_imsi_complete,
				time,
				l3_neigh_id_1_cellid,
				l3_neigh_id_2_cellid,
				l3_neigh_id_3_cellid,
				l3_neigh_sinr_1,
				l3_neigh_sinr_2,
				l3_neigh_sinr_3,
				l3_serving_id_m_cellid
			FROM cu_cp_bucket
			WHERE time > '{last_time_seen}'
			ORDER BY time ASC
		"""
		cu_points = list(client.query(cu_query).get_points())

		if not cu_points:
			time.sleep(QUERY_INTERVAL)
			continue

		du_query = """
			SELECT
				"drb_uethp_dl_ueid",
				rru_prb_used_dl,
				tb_err_total_nbr_dl_1,
				ue_imsi_complete,
				time
			FROM du_bucket
			ORDER BY time ASC
		"""
		du_points = list(client.query(du_query).get_points())
		du_lookup = {(p['ue_imsi_complete'], p['time']): p for p in du_points}

		cuup_query = """
			SELECT
				"drb_pdcp_sdu_volume_dl_filter_ueid_tx_bytes",
				"tot_pdcp_sdu_nbr_dl_ueid_tx_dl_packets",
				"drb_pdcp_sdu_delay_dl_ueid_pdcp_latency",
				ue_imsi_complete,
				time
			FROM cu_up_bucket
			ORDER BY time ASC
		"""
		cuup_points = list(client.query(cuup_query).get_points())
		cuup_lookup = {(p['ue_imsi_complete'], p['time']): p for p in cuup_points}

		for cu_point in cu_points:
			ueid = cu_point['ue_imsi_complete']
			t = cu_point['time']

			if t > last_time_seen:
				last_time_seen = t

			du_point = du_lookup.get((ueid, t))
			cuup_point = cuup_lookup.get((ueid, t))
			if not du_point or not cuup_point:
				continue

			row = {
				'ueid': ueid,
				'time': t,

				# CU-CP
				'numActiveUes': cu_point.get('num_active_ues'),
				'L3 serving SINR': cu_point.get('l3_serving_sinr'),
				'L3 serving SINR 3gpp': cu_point.get('l3_serving_sinr_3gpp'),
				'neighbor_id_1': cu_point.get('l3_neigh_id_1_cellid'),
				'neighbor_id_2': cu_point.get('l3_neigh_id_2_cellid'),
				'neighbor_id_3': cu_point.get('l3_neigh_id_3_cellid'),
				'neighbor_sinr_1': cu_point.get('l3_neigh_sinr_1'),
				'neighbor_sinr_2': cu_point.get('l3_neigh_sinr_2'),
				'neighbor_sinr_3': cu_point.get('l3_neigh_sinr_3'),
				'serving_cell_id': cu_point.get('l3_serving_id_m_cellid'),

				# DU
				'RRU.PrbUsedDl': du_point.get('rru_prb_used_dl'),
				'TB.ErrTotalNbrDl.1': du_point.get('tb_err_total_nbr_dl_1'),
				'DRB.UEThpDl.UEID': du_point.get('drb_uethp_dl_ueid'),

				# CU-UP
				'DRB.PdcpSduVolumeDl_Filter.UEID(txBytes)': cuup_point.get('drb_pdcp_sdu_volume_dl_filter_ueid_tx_bytes'),
				'Tot.PdcpSduNbrDl.UEID(txDlPackets)': cuup_point.get('tot_pdcp_sdu_nbr_dl_ueid_tx_dl_packets'),
				'DRB.PdcpSduDelayDl.UEID(pdcpLatency)': cuup_point.get('drb_pdcp_sdu_delay_dl_ueid_pdcp_latency'),
			}

			# soft-cast numerics
			for k in [
				'numActiveUes','L3 serving SINR','L3 serving SINR 3gpp',
				'RRU.PrbUsedDl','TB.ErrTotalNbrDl.1',
				'DRB.UEThpDl.UEID',
				'DRB.PdcpSduVolumeDl_Filter.UEID(txBytes)',
				'Tot.PdcpSduNbrDl.UEID(txDlPackets)',
				'DRB.PdcpSduDelayDl.UEID(pdcpLatency)',
				'serving_cell_id'
			]:
				v = row.get(k, None)
				if v is not None:
					try:
						row[k] = float(v)
					except Exception:
						pass

			yield row

		time.sleep(QUERY_INTERVAL)

# =========================
# Dry-run
# =========================
def dry_run_once():
	try:
		assigner = load_assigner_split(MODELS_DIR)
		K = int(assigner["centroids"].shape[0])
		vaes = load_cluster_vaes(MODELS_DIR, K)
		print("[DRY] clusters:", K, "| VAE packs:", sorted(list(vaes.keys())))

		row = {
			'ueid': 123456,
			'time': "1970-01-01T00:00:01Z",
			'numActiveUes': 1.0,
			'L3 serving SINR': 10.0,
			'L3 serving SINR 3gpp': 8.0,
			'neighbor_id_1': None, 'neighbor_id_2': None, 'neighbor_id_3': None,
			'neighbor_sinr_1': None, 'neighbor_sinr_2': None, 'neighbor_sinr_3': None,
			'RRU.PrbUsedDl': 0.1,
			'TB.ErrTotalNbrDl.1': 0.0,
			'DRB.UEThpDl.UEID': 50.0,
			'DRB.PdcpSduVolumeDl_Filter.UEID(txBytes)': 1000.0,
			'Tot.PdcpSduNbrDl.UEID(txDlPackets)': 10.0,
			'DRB.PdcpSduDelayDl.UEID(pdcpLatency)': 5.0,
		}

		k = assign_cluster_for_row(row, assigner)
		print("[DRY] assigned cluster:", k)
		if k not in vaes:
			print("[DRY][WARN] No VAE pack for cluster", k)
			return
		rec_err, y_hat = vae_anomaly_for_row(row, vaes[k])
		print("[DRY] rec_err={:.6f}  thr={:.6f}  pred={}".format(rec_err, vaes[k]['threshold'], y_hat))
	except Exception as e:
		print("[DRY][ERROR]", e)
		traceback.print_exc()

# =========================
# Main
# =========================
def main():
	try:
		print("[INIT] MODELS_DIR:", MODELS_DIR.resolve())
		if not MODELS_DIR.exists():
			print("[INIT][ERROR] MODELS_DIR not found!")
		else:
			try:
				print("[INIT] MODELS_DIR contents:", os.listdir(MODELS_DIR))
			except Exception as e:
				print("[INIT][WARN] listdir failed:", e)

		print("[INIT] Loading assigner (split files)...")
		assigner = load_assigner_split(MODELS_DIR)
		K = int(assigner["centroids"].shape[0])
		print("[INIT] Clusters (K):", K)
		print("[CHECK] distance_metric:", assigner["distance_metric"])
		print("[CHECK] centroids shape:", assigner["centroids"].shape)
		print("[CHECK] invs len:", len(assigner["invs"]))

		print("[INIT] Loading VAEs...")
		vaes = load_cluster_vaes(MODELS_DIR, K)
		print("[INIT] Loaded VAE packs:", sorted(list(vaes.keys())))

		print("[INIT] Running DRY-RUN once...")
		dry_run_once()
		print("[INIT] DRY-RUN done.")

		print("\n[RUN] Starting stream (every {}s)...\n".format(QUERY_INTERVAL))
		print("Time                          | UEID   | Cluster |  rec_err  |    thr   | ServingSINR | Neigh( id:sinr, ... )")
		print("-" * 130)

		for row in query_influxdb():
			try:
				if DEBUG:
					print("[ROW] keys:", list(row.keys())[:8], "... total:", len(row))

				k = assign_cluster_for_row(row, assigner)
				if k not in vaes:
					if DEBUG:
						print("[WARN] No VAE pack for cluster {}. Skipping row.".format(k))
					continue

				rec_err, y_hat = vae_anomaly_for_row(row, vaes[k])
				thr = vaes[k]['threshold']

				serving_sinr = row.get('L3 serving SINR', None)
				s_sinr_str = "{:.2f}".format(serving_sinr) if isinstance(serving_sinr, (int, float)) else "N/A"
				neigh_str = "{}:{}, {}:{}, {}:{}".format(
					row.get('neighbor_id_1'), row.get('neighbor_sinr_1'),
					row.get('neighbor_id_2'), row.get('neighbor_sinr_2'),
					row.get('neighbor_id_3'), row.get('neighbor_sinr_3')
				)

				print("{} | {:6d} | {:^7d} | {:9.4f} | {:9.4f} | {:>11} | {}".format(
					row['time'], int(row['ueid']), k, rec_err, thr, s_sinr_str, neigh_str))

				if y_hat == 1:

					print(
						"\n[ALERT] UE {} anomalous at {} (cluster={}, rec_err={:.4f}, thr={:.4f}); "
						"ServingSINR={}; Neigh={}\n".format(
							int(row['ueid']), row['time'], k, rec_err, thr, s_sinr_str, neigh_str
						)
					)

					expected_cell = CLUSTER_TO_CELLID.get(int(k))
					current_cell = row.get('serving_cell_id', None)

					try:
						current_cell = int(float(current_cell)) if current_cell is not None else None
					except Exception:
						current_cell = None

					if current_cell is None or expected_cell is None:
						if DEBUG:
							print(f"[SKIP-LOG] UE {int(row['ueid'])}: missing cell info "
								  f"(current_cell={current_cell}, expected_cell={expected_cell})")
					else:
						if current_cell != int(expected_cell):
							log_line = (
								"Mismatch: UE ID {ueid} | time {t} | "
								"Cluster={k}({kname}) -> expected CellID {exp}({expn}) | "
								"Current CellID {cur}({curn})"
							).format(
								ueid=int(row['ueid']),
								t=row['time'],
								k=int(k), kname=CLUSTER_NAME.get(int(k), "unknown"),
								exp=int(expected_cell), expn=CELLID_NAME.get(int(expected_cell), "unknown"),
								cur=int(current_cell), curn=CELLID_NAME.get(int(current_cell), "unknown"),
							)
							logging.warning(log_line)
							if DEBUG:
								print("[LOGGED] " + log_line)
						else:
							if DEBUG:
								print(f"[OK-MATCH] UE {int(row['ueid'])}: cluster {k} ↔ CellID {current_cell} "
									  f"({CELLID_NAME.get(int(current_cell), 'unknown')})")

				time.sleep(QUERY_INTERVAL)

			except Exception as inner_e:
				print("[ERROR] Iteration error:", inner_e)
				traceback.print_exc()
				try:
					from pprint import pformat
					print("[ERROR] Offending row dump:\n", pformat(row))
				except Exception:
					pass
				time.sleep(0.5)

	except KeyboardInterrupt:
		print("\nShutting down...")
	except Exception as outer_e:
		print("[CRITICAL] Fatal error:", outer_e)
		traceback.print_exc()
		try:
			print("[CRITICAL] MODELS_DIR contents:", os.listdir(MODELS_DIR))
		except Exception:
			pass
# =========================
# Entry
# =========================
if __name__ == "__main__":
	os.environ.setdefault("PYTHONUNBUFFERED", "1")
	print("[BOOT] __main__ guard active. Calling main() ...", flush=True)
	main()