import os
import re
from traceback import format_exc
from influxdb import InfluxDBClient
from metric_src.metric_utils import MetricUtils


class InfluxUtils:

	def __init__(self):
		
		self.initiate_connection()
		self.metric_utils = MetricUtils()

	def initiate_connection(self):

		influx_config = {
			"host": os.environ.get("INFLUX_HOST") or "influxdb",
			"port": os.environ.get("INFLUX_PORT") or 8086,
			"database": os.environ.get("INFLUX_DB") or "ns3_metrics"}
		
		self.influx_client = InfluxDBClient(
			host=influx_config["host"],
			port=influx_config["port"])
		
		databases = self.influx_client.get_list_database()
		database_names = [db['name'] for db in databases]

		if influx_config["database"] not in database_names:
			self.influx_client.create_database(
				dbname=influx_config["database"])
			
		self.influx_client.switch_database(
			database=influx_config["database"])

	def insert_influx_data(self, data: dict):

		try:
			self.influx_client.write_points(points=[data])
		
		except:
			print(f"Error in InfluxDB insertion:\n\n{format_exc()}")

	def process_row(self, row: dict, file_name: str):

		row = {
			k.strip(): v.strip()
			for k, v in row.items()
			if k is not None and k.strip() != ""}

		if re.search('cu-cp-cell-[2-9].txt', file_name):

			influx_data = self.metric_utils.structure_influx_data(
				row=row, row_type="cu_cp")
			
			influx_cell_data = self.metric_utils.structure_influx_cell_data(
				cu_cp_data=influx_data)
			
			self.insert_influx_data(data=influx_data)
			self.insert_influx_data(data=influx_cell_data)

		elif re.search('cu-up-cell-[2-9].txt', file_name):

			influx_data = self.metric_utils.structure_influx_data(
				row=row, row_type="cu_up")
			
			self.insert_influx_data(data=influx_data)

		elif re.search('du-cell-[2-9].txt', file_name):

			influx_data = self.metric_utils.structure_influx_data(
				row=row, row_type="du")
			
			self.insert_influx_data(data=influx_data)

		else:
			print(f"File type ({file_name}) not supported")
