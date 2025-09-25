import yaml
import math
from pathlib import Path
from datetime import datetime, timezone


class MetricUtils:

	def __init__(self):

		self.field_maps = self.load_field_maps()

	def load_field_maps(self):

		file_path = Path(__file__).resolve().parent.joinpath("field_maps.yml")
		with open(file_path, "r") as input_file:
			field_maps = yaml.full_load(input_file)

		return field_maps
	
	def safe_float(self, value: float):

		try:
			f = float(value)
			if math.isinf(f) or math.isnan(f):
				return 0.0
			else:
				return f
			
		except (ValueError, TypeError):
			return 0.0

	def parse_numeric_value(self, value: str):

		try:
			v = value.strip()
			if v == "":
				return 0.0
			else:
				return self.safe_float(v)
			
		except (ValueError, AttributeError):
			return 0.0

	def structure_influx_time(self, csv_timestamp: str):

		csv_timestamp_ms = int(csv_timestamp.strip())

		csv_timestamp_sec = csv_timestamp_ms / 1000
		original_dt = datetime.fromtimestamp(csv_timestamp_sec, tz=timezone.utc)
		current_dt = datetime.now(timezone.utc)
		modified_dt = original_dt.replace(
			year=current_dt.year,
			month=current_dt.month,
			day=current_dt.day)
		
		modified_timestamp_ns = int(modified_dt.timestamp() * 1_000_000_000)

		return modified_timestamp_ns

	def structure_influx_data(self, row: dict, row_type: str):

		influx_fields = {}
		for key, value in row.items():
			if key == "timestamp":
				continue

			influx_key = self.field_maps[row_type].get(key.strip())
			influx_value = self.parse_numeric_value(value)

			influx_fields[influx_key] = influx_value

		tags = {"ueImsiComplete": str(row["ueImsiComplete"])}
		if row_type == "cu_cp":
			tags["L3ServingId"] = str(row["L3 serving Id(m_cellId)"])
			influx_fields["is_anomaly"] = 0.0
		
		elif row_type == "du":
			tags["nrCellId"] = str(row["nrCellId"])

		influx_data = {
			"measurement": f"{row_type}_bucket",
			"tags": tags,
			"fields": influx_fields,
			"time": self.structure_influx_time(row["timestamp"])}
		
		return influx_data

	def structure_influx_cell_data(self, cu_cp_data: dict):

		influx_fields = {"ue_imsi_complete": cu_cp_data["fields"]["ue_imsi_complete"]}
		for index in range(2, 8):
			influx_fields[f"cell_{index}_sinr"] = 0.0

		for key, value in self.field_maps["ue_cell"].items():
			cell_id = int(cu_cp_data["fields"][key])
			sinr = cu_cp_data["fields"][value]

			if cell_id in range(2, 8):
				influx_fields[f"cell_{cell_id}_sinr"] = sinr

		tags = {"ueImsiComplete": cu_cp_data["tags"]["ueImsiComplete"]}

		influx_cell_data = {
			"measurement": "ue_cell_bucket",
			"tags": tags,
			"fields": influx_fields,
			"time": cu_cp_data["time"]}
		
		return influx_cell_data
