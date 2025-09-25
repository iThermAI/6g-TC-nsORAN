/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/* *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Authors: Andrea Lacava <thecave003@gmail.com>
 * Michele Polese <michele.polese@gmail.com>
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-helper.h"
#include <ns3/lte-ue-net-device.h>
#include "ns3/mmwave-helper.h"
#include "ns3/epc-helper.h"
#include "ns3/mmwave-point-to-point-epc-helper.h"
#include "ns3/lte-helper.h"
#include <fstream> // For file output
#include <map>     // For mapping UE ID to output file
#include <string>  // For string manipulation
#include <cmath>   // For std::sqrt

using namespace ns3;
using namespace mmwave;

/**
 * Scenario Zero
 * */

NS_LOG_COMPONENT_DEFINE ("ScenarioZero");

// Global variables for data rate reporting
// We'll map internal Node ID to the PacketSink, as GetId() is unique
std::map<uint32_t, Ptr<PacketSink>> g_uePacketSinks;
// Files will be mapped by Node ID, but named by slice type and index
std::map<uint32_t, std::ofstream*> g_ueDataRateFiles;
std::map<uint32_t, double> g_ueLastTotalRxBytes;
std::map<uint32_t, double> g_ueLastThroughputTime;
// Map Node ID to a more user-friendly string (e.g., "urllc_ue_0")
std::map<uint32_t, std::string> g_ueIdToSliceName;

std::string g_outputDir = "."; // Default output directory
double g_reportingInterval = 0.5; // Report every 0.5 seconds

// Function to calculate and report throughput for each UE
void
CalculateThroughput (const NodeContainer& ueNodes, Time reportInterval)
{
  for (uint32_t i = 0; i < ueNodes.GetN (); ++i)
    {
      Ptr<Node> ueNode = ueNodes.Get (i);
      Ptr<PacketSink> sink = g_uePacketSinks[ueNode->GetId ()];

      if (sink)
        {
          double currentTotalRxBytes = sink->GetTotalRx ();
          double currentTime = Simulator::Now ().GetSeconds ();

          // Retrieve last values for this specific UE
          double lastTotalRxBytes = g_ueLastTotalRxBytes[ueNode->GetId ()];
          double lastThroughputTime = g_ueLastThroughputTime[ueNode->GetId ()];

          // Calculate instantaneous throughput since last report for this UE
          double intervalBytes = currentTotalRxBytes - lastTotalRxBytes;
          double intervalTime = currentTime - lastThroughputTime;

          if (intervalTime > 0)
            {
              double throughputMbps = (intervalBytes * 8.0) / (intervalTime * 1000000.0);

              // Get the correct output file for this UE
              std::ofstream* outFile = g_ueDataRateFiles[ueNode->GetId ()];
              if (outFile && outFile->is_open ())
                {
                  *outFile << currentTime << "\t" << throughputMbps << std::endl;
                  // Log with the friendly slice name
                  NS_LOG_UNCOND ("UE " << g_ueIdToSliceName[ueNode->GetId ()] << " (Node ID: " << ueNode->GetId() << ") Throughput: " << throughputMbps << " Mbps at time " << currentTime << "s");
                }
            }
          // Update last values for this specific UE for the next interval
          g_ueLastTotalRxBytes[ueNode->GetId ()] = currentTotalRxBytes;
          g_ueLastThroughputTime[ueNode->GetId ()] = currentTime;
        }
    }
  
  Simulator::Schedule (reportInterval, &CalculateThroughput, std::cref(ueNodes), reportInterval);
}


void
PrintGnuplottableUeListToFile (std::string filename)
{
  std::ofstream outFile;
  outFile.open (filename.c_str (), std::ios_base::out | std::ios_base::trunc);
  if (!outFile.is_open ())
    {
      NS_LOG_ERROR ("Can't open file " << filename);
      return;
    }
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<Node> node = *it;
      int nDevs = node->GetNDevices ();
      for (int j = 0; j < nDevs; j++)
        {
          Ptr<LteUeNetDevice> uedev = node->GetDevice (j)->GetObject<LteUeNetDevice> ();
          Ptr<MmWaveUeNetDevice> mmuedev = node->GetDevice (j)->GetObject<MmWaveUeNetDevice> ();
          Ptr<McUeNetDevice> mcuedev = node->GetDevice (j)->GetObject<McUeNetDevice> ();
          // Only print UEs that are part of our defined slice groups
          if (uedev || mmuedev || mcuedev)
            {
              // Check if this UE is one of our simulated UEs by looking it up in the map
              auto it_map = g_ueIdToSliceName.find(node->GetId());
              if (it_map != g_ueIdToSliceName.end())
              {
                  Vector pos = node->GetObject<MobilityModel> ()->GetPosition ();
                  // Use the user-friendly slice name for the label
                  outFile << "set label \"" << it_map->second << "\" at " << pos.x << "," << pos.y
                          << " left font \"Helvetica,8\" textcolor rgb \"black\" front point pt 1 ps "
                             "0.3 lc rgb \"black\" offset 0,0"
                          << std::endl;
              }
            }
        }
    }
}

void
PrintGnuplottableEnbListToFile (std::string filename)
{
  std::ofstream outFile;
  outFile.open (filename.c_str (), std::ios_base::out | std::ios_base::trunc);
  if (!outFile.is_open ())
    {
      NS_LOG_ERROR ("Can't open file " << filename);
      return;
    }
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<Node> node = *it;
      int nDevs = node->GetNDevices ();
      for (int j = 0; j < nDevs; j++)
        {
          Ptr<LteEnbNetDevice> enbdev = node->GetDevice (j)->GetObject<LteEnbNetDevice> ();
          Ptr<MmWaveEnbNetDevice> mmdev = node->GetDevice (j)->GetObject<MmWaveEnbNetDevice> ();
          if (enbdev)
            {
              Vector pos = node->GetObject<MobilityModel> ()->GetPosition ();
              outFile << "set label \"" << enbdev->GetCellId () << "\" at " << pos.x << "," << pos.y
                      << " left font \"Helvetica,8\" textcolor rgb \"blue\" front  point pt 4 ps "
                         "0.3 lc rgb \"blue\" offset 0,0"
                      << std::endl;
            }
          else if (mmdev)
            {
              Vector pos = node->GetObject<MobilityModel> ()->GetPosition ();
              outFile << "set label \"" << mmdev->GetCellId () << "\" at " << pos.x << "," << pos.y
                      << " left font \"Helvetica,8\" textcolor rgb \"red\" front  point pt 4 ps "
                         "0.3 lc rgb \"red\" offset 0,0"
                      << std::endl;
            }
        }
    }
}

void
PrintPosition (Ptr<Node> node)
{
  Ptr<MobilityModel> model = node->GetObject<MobilityModel> ();
  NS_LOG_UNCOND ("Position +****************************** " << model->GetPosition () << " at time "
                                                              << Simulator::Now ().GetSeconds ());
}

static ns3::GlobalValue g_bufferSize ("bufferSize", "RLC tx buffer size (MB)",
                                       ns3::UintegerValue (10),
                                       ns3::MakeUintegerChecker<uint32_t> ());

static ns3::GlobalValue g_enableTraces ("enableTraces", "If true, generate ns-3 traces",
                                         ns3::BooleanValue (true), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2lteEnabled ("e2lteEnabled", "If true, send LTE E2 reports",
                                         ns3::BooleanValue (true), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2nrEnabled ("e2nrEnabled", "If true, send NR E2 reports",
                                        ns3::BooleanValue (true), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2du ("e2du", "If true, send DU reports", ns3::BooleanValue (true),
                                ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2cuUp ("e2cuUp", "If true, send CU-UP reports", ns3::BooleanValue (true),
                                  ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2cuCp ("e2cuCp", "If true, send CU-CP reports", ns3::BooleanValue (true),
                                  ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_reducedPmValues ("reducedPmValues", "If true, use a subset of the the pm containers",
                                         ns3::BooleanValue (true), ns3::MakeBooleanChecker ());

static ns3::GlobalValue
    g_hoSinrDifference ("hoSinrDifference",
                        "The value for which an handover between MmWave eNB is triggered",
                        ns3::DoubleValue (3), ns3::MakeDoubleChecker<double> ());

static ns3::GlobalValue
    g_indicationPeriodicity ("indicationPeriodicity",
                             "E2 Indication Periodicity reports (value in seconds)",
                             ns3::DoubleValue (0.01), ns3::MakeDoubleChecker<double> (0.01, 2.0));

static ns3::GlobalValue g_simTime ("simTime", "Simulation time in seconds", ns3::DoubleValue (2),
                                    ns3::MakeDoubleChecker<double> (0.1, 100.0));

static ns3::GlobalValue g_outageThreshold ("outageThreshold",
                                            "SNR threshold for outage events [dB]", // use -1000.0 with NoAuto
                                            ns3::DoubleValue (-50.0),
                                            ns3::MakeDoubleChecker<double> ());

static ns3::GlobalValue
    g_numberOfRaPreambles ("numberOfRaPreambles",
                           "how many random access preambles are available for the contention based RACH process",
                           ns3::UintegerValue (40), // Indicated for TS use case, 52 is default
                           ns3::MakeUintegerChecker<uint8_t> ());

static ns3::GlobalValue
    g_handoverMode ("handoverMode",
                    "HO euristic to be used,"
                    "can be only \"NoAuto\", \"FixedTtt\", \"DynamicTtt\",   \"Threshold\"",
                    ns3::StringValue ("DynamicTtt"), ns3::MakeStringChecker ());

static ns3::GlobalValue g_e2TermIp ("e2TermIp", "The IP address of the RIC E2 termination",
                                     ns3::StringValue ("10.0.2.10"), ns3::MakeStringChecker ());

static ns3::GlobalValue
    g_enableE2FileLogging ("enableE2FileLogging",
                           "If true, generate offline file logging instead of connecting to RIC",
                           ns3::BooleanValue (false), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_controlFileName ("controlFileName",
                                            "The path to the control file (can be absolute)",
                                            ns3::StringValue (""),
                                            ns3::MakeStringChecker ());

static ns3::GlobalValue q_useSemaphores ("useSemaphores", "If true, enables the use of semaphores for external environment control",
                                         ns3::BooleanValue (false), ns3::MakeBooleanChecker ());

int
main (int argc, char *argv[])
{
  LogComponentEnableAll (LOG_PREFIX_ALL);
  // LogComponentEnable ("RicControlMessage", LOG_LEVEL_ALL);
  // LogComponentEnable ("Asn1Types", LOG_LEVEL_LOGIC);
  // LogComponentEnable ("E2Termination", LOG_LEVEL_LOGIC);

  // LogComponentEnable ("LteEnbNetDevice", LOG_LEVEL_ALL);
  // LogComponentEnable ("MmWaveEnbNetDevice", LOG_LEVEL_DEBUG);

  // The maximum X coordinate of the scenario
  double maxXAxis = 4000;
  // The maximum Y coordinate of the scenario
  double maxYAxis = 4000;

  // Command line arguments
  CommandLine cmd;
  cmd.AddValue ("outputDir", "Output directory for traces and reports", g_outputDir);
  cmd.AddValue ("reportingInterval", "Interval for data rate reporting (seconds)", g_reportingInterval);
  cmd.Parse (argc, argv);

  bool harqEnabled = true;

  UintegerValue uintegerValue;
  BooleanValue booleanValue;
  StringValue stringValue;
  DoubleValue doubleValue;

  GlobalValue::GetValueByName ("hoSinrDifference", doubleValue);
  double hoSinrDifference = doubleValue.Get ();
  GlobalValue::GetValueByName ("bufferSize", uintegerValue);
  uint32_t bufferSize = uintegerValue.Get ();
  GlobalValue::GetValueByName ("enableTraces", booleanValue);
  bool enableTraces = booleanValue.Get ();
  GlobalValue::GetValueByName ("outageThreshold", doubleValue);
  double outageThreshold = doubleValue.Get ();
  GlobalValue::GetValueByName ("handoverMode", stringValue);
  std::string handoverMode = stringValue.Get ();
  GlobalValue::GetValueByName ("e2TermIp", stringValue);
  std::string e2TermIp = stringValue.Get ();
  GlobalValue::GetValueByName ("enableE2FileLogging", booleanValue);
  bool enableE2FileLogging = booleanValue.Get ();
  GlobalValue::GetValueByName ("numberOfRaPreambles", uintegerValue);
  uint8_t numberOfRaPreambles = uintegerValue.Get ();

  NS_LOG_UNCOND ("bufferSize " << bufferSize << " OutageThreshold " << outageThreshold
                               << " HandoverMode " << handoverMode << " e2TermIp " << e2TermIp
                               << " enableE2FileLogging " << enableE2FileLogging);

  GlobalValue::GetValueByName ("e2lteEnabled", booleanValue);
  bool e2lteEnabled = booleanValue.Get ();

  GlobalValue::GetValueByName ("e2nrEnabled", booleanValue);
  bool e2nrEnabled = booleanValue.Get ();

  GlobalValue::GetValueByName ("e2du", booleanValue);
  bool e2du = booleanValue.Get ();

  GlobalValue::GetValueByName ("e2cuUp", booleanValue);
  bool e2cuUp = booleanValue.Get ();

  GlobalValue::GetValueByName ("e2cuCp", booleanValue);
  bool e2cuCp = booleanValue.Get ();

  GlobalValue::GetValueByName ("reducedPmValues", booleanValue);
  bool reducedPmValues = booleanValue.Get ();

  GlobalValue::GetValueByName ("indicationPeriodicity", doubleValue);
  double indicationPeriodicity = doubleValue.Get ();

  GlobalValue::GetValueByName ("controlFileName", stringValue);
  std::string controlFilename = stringValue.Get ();

  GlobalValue::GetValueByName ("useSemaphores", booleanValue);
  bool useSemaphores = booleanValue.Get ();


  NS_LOG_UNCOND ("e2lteEnabled " << e2lteEnabled << " e2nrEnabled " << e2nrEnabled << " e2du "
                                 << e2du << " e2cuCp " << e2cuCp << " e2cuUp " << e2cuUp
                                 << " controlFilename " << controlFilename
                                 << " useSemaphores " << useSemaphores
                                 << " indicationPeriodicity " << indicationPeriodicity);

  Config::SetDefault ("ns3::LteEnbNetDevice::UseSemaphores", BooleanValue (useSemaphores));
  Config::SetDefault ("ns3::LteEnbNetDevice::ControlFileName", StringValue (controlFilename));
  Config::SetDefault ("ns3::LteEnbNetDevice::E2Periodicity", DoubleValue (indicationPeriodicity));
  Config::SetDefault ("ns3::MmWaveEnbNetDevice::E2Periodicity",
                      DoubleValue (indicationPeriodicity));

  Config::SetDefault ("ns3::MmWaveHelper::E2ModeLte", BooleanValue (e2lteEnabled));
  Config::SetDefault ("ns3::MmWaveHelper::E2ModeNr", BooleanValue (e2nrEnabled));
  Config::SetDefault ("ns3::MmWaveHelper::E2Periodicity", DoubleValue (indicationPeriodicity));

  // The DU PM reports should come from both NR gNB as well as LTE eNB,
  // since in the RLC/MAC/PHY entities are present in BOTH NR gNB as well as LTE eNB.
  // DU reports from LTE eNB are not implemented in this release
  Config::SetDefault ("ns3::MmWaveEnbNetDevice::EnableDuReport", BooleanValue (e2du));

  // The CU-UP PM reports should only come from LTE eNB, since in the NS3 “EN-DC
  // simulation (Option 3A)”, the PDCP is only in the LTE eNB and NOT in the NR gNB
  Config::SetDefault ("ns3::MmWaveEnbNetDevice::EnableCuUpReport", BooleanValue (e2cuUp));
  Config::SetDefault ("ns3::LteEnbNetDevice::EnableCuUpReport", BooleanValue (e2cuUp));

  Config::SetDefault ("ns3::MmWaveEnbNetDevice::EnableCuCpReport", BooleanValue (e2cuCp));
  Config::SetDefault ("ns3::LteEnbNetDevice::EnableCuCpReport", BooleanValue (e2cuCp));

  Config::SetDefault ("ns3::MmWaveEnbNetDevice::ReducedPmValues", BooleanValue (reducedPmValues));
  Config::SetDefault ("ns3::LteEnbNetDevice::ReducedPmValues", BooleanValue (reducedPmValues));

  Config::SetDefault ("ns3::LteEnbNetDevice::EnableE2FileLogging",
                      BooleanValue (enableE2FileLogging));
  Config::SetDefault ("ns3::MmWaveEnbNetDevice::EnableE2FileLogging",
                      BooleanValue (enableE2FileLogging));

  Config::SetDefault ("ns3::MmWaveEnbMac::NumberOfRaPreambles",
                      UintegerValue (numberOfRaPreambles));

  Config::SetDefault ("ns3::MmWaveHelper::HarqEnabled", BooleanValue (harqEnabled));
  Config::SetDefault ("ns3::MmWaveHelper::UseIdealRrc", BooleanValue (true));
  Config::SetDefault ("ns3::MmWaveHelper::E2TermIp", StringValue (e2TermIp));

  Config::SetDefault ("ns3::MmWaveFlexTtiMacScheduler::HarqEnabled", BooleanValue (harqEnabled));
  Config::SetDefault ("ns3::MmWavePhyMacCommon::NumHarqProcess", UintegerValue (100));
  //Config::SetDefault ("ns3::MmWaveBearerStatsCalculator::EpochDuration", TimeValue (MilliSeconds (10.0)));

  // set to false to use the 3GPP radiation pattern (proper configuration of the bearing and downtilt angles is needed)
  Config::SetDefault ("ns3::ThreeGppChannelModel::UpdatePeriod", TimeValue (MilliSeconds (100.0)));
  Config::SetDefault ("ns3::ThreeGppChannelConditionModel::UpdatePeriod",
                      TimeValue (MilliSeconds (100)));

  Config::SetDefault ("ns3::LteRlcAm::ReportBufferStatusTimer", TimeValue (MilliSeconds (10.0)));
  Config::SetDefault ("ns3::LteRlcUmLowLat::ReportBufferStatusTimer",
                      TimeValue (MilliSeconds (10.0)));
  Config::SetDefault ("ns3::LteRlcUm::MaxTxBufferSize", UintegerValue (bufferSize * 1024 * 1024));
  Config::SetDefault ("ns3::LteRlcUmLowLat::MaxTxBufferSize",
                      UintegerValue (bufferSize * 1024 * 1024));
  Config::SetDefault ("ns3::LteRlcAm::MaxTxBufferSize", UintegerValue (bufferSize * 1024 * 1024));

  Config::SetDefault ("ns3::LteEnbRrc::OutageThreshold", DoubleValue (outageThreshold));
  Config::SetDefault ("ns3::LteEnbRrc::SecondaryCellHandoverMode", StringValue (handoverMode));
  Config::SetDefault ("ns3::LteEnbRrc::HoSinrDifference", DoubleValue (hoSinrDifference));

  // Carrier bandwidth in Hz
  double bandwidth = 10e6;
  // Center frequency in Hz
  double centerFrequency = 3.5e9;
  // Distance between the mmWave BSs and the two co-located LTE and mmWave BSs in meters
  double isd = 500; // (interside distance)
  // Number of antennas in each UE
  int numAntennasMcUe = 1;
  // Number of antennas in each mmWave BS
  int numAntennasMmWave = 1;

  NS_LOG_INFO ("Bandwidth " << bandwidth << " centerFrequency " << double (centerFrequency)
                            << " isd " << isd << " numAntennasMcUe " << numAntennasMcUe
                            << " numAntennasMmWave " << numAntennasMmWave);

  Config::SetDefault ("ns3::MmWavePhyMacCommon::Bandwidth", DoubleValue (bandwidth));
  Config::SetDefault ("ns3::MmWavePhyMacCommon::CenterFreq", DoubleValue (centerFrequency));

  Ptr<MmWaveHelper> mmwaveHelper = CreateObject<MmWaveHelper> ();
  mmwaveHelper->SetPathlossModelType ("ns3::ThreeGppUmiStreetCanyonPropagationLossModel");
  mmwaveHelper->SetChannelConditionModelType ("ns3::ThreeGppUmiStreetCanyonChannelConditionModel");

  // Set the number of antennas in the devices
  mmwaveHelper->SetUePhasedArrayModelAttribute("NumColumns", UintegerValue(std::sqrt(numAntennasMcUe)));
  mmwaveHelper->SetUePhasedArrayModelAttribute("NumRows", UintegerValue(std::sqrt(numAntennasMcUe)));
  mmwaveHelper->SetEnbPhasedArrayModelAttribute("NumColumns",UintegerValue(std::sqrt(numAntennasMmWave)));
  mmwaveHelper->SetEnbPhasedArrayModelAttribute("NumRows", UintegerValue(std::sqrt(numAntennasMmWave)));

  Ptr<MmWavePointToPointEpcHelper> epcHelper = CreateObject<MmWavePointToPointEpcHelper> ();
  mmwaveHelper->SetEpcHelper (epcHelper);

  uint8_t nMmWaveEnbNodes = 3;
  uint8_t nLteEnbNodes = 1;
  uint32_t ues = 12; // UEs per mmWave ENB node (original logic implies 3 UEs per mmWave ENB)
  uint32_t nUeNodes = ues * nMmWaveEnbNodes; // Total UEs

  NS_LOG_INFO (" Bandwidth " << bandwidth << " centerFrequency " << double (centerFrequency)
                              << " isd " << isd << " numAntennasMcUe " << numAntennasMcUe
                              << " numAntennasMmWave " << numAntennasMmWave << " nMmWaveEnbNodes "
                              << unsigned (nMmWaveEnbNodes) << " nUeNodes " << nUeNodes);

  // Get SGW/PGW and create a single RemoteHost
  Ptr<Node> pgw = epcHelper->GetPgwNode ();
  NodeContainer remoteHostContainer;
  remoteHostContainer.Create (1);
  Ptr<Node> remoteHost = remoteHostContainer.Get (0);
  InternetStackHelper internet;
  internet.Install (remoteHostContainer);

  // Create the Internet by connecting remoteHost to pgw. Setup routing too
  PointToPointHelper p2ph;
  p2ph.SetDeviceAttribute ("DataRate", DataRateValue (DataRate ("100Gb/s")));
  p2ph.SetDeviceAttribute ("Mtu", UintegerValue (10000));
  p2ph.SetChannelAttribute ("Delay", TimeValue (Seconds (0.010)));
  NetDeviceContainer internetDevices = p2ph.Install (pgw, remoteHost);
  Ipv4AddressHelper ipv4h;
  ipv4h.SetBase ("1.0.0.0", "255.0.0.0");
  Ipv4InterfaceContainer internetIpIfaces = ipv4h.Assign (internetDevices);
  // interface 0 is localhost, 1 is the p2p device
  Ipv4Address remoteHostAddr = internetIpIfaces.GetAddress (1);
  Ipv4StaticRoutingHelper ipv4RoutingHelper;
  Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
      ipv4RoutingHelper.GetStaticRouting (remoteHost->GetObject<Ipv4> ());
  remoteHostStaticRouting->AddNetworkRouteTo (Ipv4Address ("7.0.0.0"), Ipv4Mask ("255.0.0.0"), 1);

  // create LTE, mmWave eNB nodes and UE node
  NodeContainer ueNodes;
  NodeContainer mmWaveEnbNodes;
  NodeContainer lteEnbNodes;
  NodeContainer allEnbNodes;
  mmWaveEnbNodes.Create (nMmWaveEnbNodes);
  lteEnbNodes.Create (nLteEnbNodes);
  ueNodes.Create (nUeNodes); // This creates the nodes with their actual IDs
  allEnbNodes.Add (lteEnbNodes);
  allEnbNodes.Add (mmWaveEnbNodes);

  // Position
  Vector centerPosition = Vector (maxXAxis / 2, maxYAxis / 2, 3);

  // Install Mobility Model
  Ptr<ListPositionAllocator> enbPositionAlloc = CreateObject<ListPositionAllocator> ();

  // We want a center with one LTE enb and one mmWave co-located in the same place
  enbPositionAlloc->Add (centerPosition);
  enbPositionAlloc->Add (centerPosition);

  double x, y;
  double nConstellation = nMmWaveEnbNodes - 1;

  // This guarantee that each of the rest BSs is placed at the same distance from the two co-located in the center
  for (int8_t i = 0; i < nConstellation; ++i)
    {
      x = isd * cos ((2 * M_PI * i) / (nConstellation));
      y = isd * sin ((2 * M_PI * i) / (nConstellation));
      enbPositionAlloc->Add (Vector (centerPosition.x + x, centerPosition.y + y, 3));
    }

  MobilityHelper enbmobility;
  enbmobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  enbmobility.SetPositionAllocator (enbPositionAlloc);
  enbmobility.Install (allEnbNodes);

  MobilityHelper uemobility;

  Ptr<UniformDiscPositionAllocator> uePositionAlloc = CreateObject<UniformDiscPositionAllocator> ();

  uePositionAlloc->SetX (centerPosition.x);
  uePositionAlloc->SetY (centerPosition.y);
  uePositionAlloc->SetRho (isd);
  Ptr<UniformRandomVariable> speed = CreateObject<UniformRandomVariable> ();
  speed->SetAttribute ("Min", DoubleValue (35));
  speed->SetAttribute ("Max", DoubleValue (35));

  uemobility.SetMobilityModel ("ns3::RandomWalk2dOutdoorMobilityModel", "Speed",
                               PointerValue (speed), "Bounds",
                               RectangleValue (Rectangle (0, maxXAxis, 0, maxYAxis)));
  uemobility.SetPositionAllocator (uePositionAlloc);
  uemobility.Install (ueNodes);

  // Install mmWave, lte, mc Devices to the nodes
  NetDeviceContainer lteEnbDevs = mmwaveHelper->InstallLteEnbDevice (lteEnbNodes);
  NetDeviceContainer mmWaveEnbDevs = mmwaveHelper->InstallEnbDevice (mmWaveEnbNodes);
  NetDeviceContainer mcUeDevs = mmwaveHelper->InstallMcUeDevice (ueNodes);

  // Install the IP stack on the UEs
  internet.Install (ueNodes);
  Ipv4InterfaceContainer ueIpIface;
  ueIpIface = epcHelper->AssignUeIpv4Address (NetDeviceContainer (mcUeDevs));
  // Assign IP address to UEs, and install applications
  for (uint32_t u = 0; u < ueNodes.GetN (); ++u)
    {
      Ptr<Node> ueNode = ueNodes.Get (u);
      // Set the default gateway for the UE
      Ptr<Ipv4StaticRouting> ueStaticRouting =
          ipv4RoutingHelper.GetStaticRouting (ueNode->GetObject<Ipv4> ());
      ueStaticRouting->SetDefaultRoute (epcHelper->GetUeDefaultGatewayAddress (), 1);
    }

  // Add X2 interfaces
  mmwaveHelper->AddX2Interface (lteEnbNodes, mmWaveEnbNodes);

  // Manual attachment
  mmwaveHelper->AttachToClosestEnb (mcUeDevs, mmWaveEnbDevs, lteEnbDevs);

  // Install and start applications
  // On the remoteHost there is UDP OnOff Application

  uint16_t portUdp = 1234; // Use a common port for all UE sinks
  Address sinkLocalAddressUdp (InetSocketAddress (Ipv4Address::GetAny (), portUdp));
  PacketSinkHelper sinkHelperUdp ("ns3::UdpSocketFactory", sinkLocalAddressUdp);
  AddressValue serverAddressUdp (InetSocketAddress (remoteHostAddr, portUdp));

  ApplicationContainer remoteHostSinkApp;
  remoteHostSinkApp.Add (sinkHelperUdp.Install (remoteHost));

  ApplicationContainer clientApp;
  ApplicationContainer ueSinkApp; // Container for UE PacketSinks

  // --- Slicing Implementation ---
  // Divide UEs into 3 groups: URLLC, eMBB, mMTC
  // Assuming total UEs are N, each group will have N/3 UEs.
  // Make sure nUeNodes is divisible by 3 for an even distribution.
  NS_ASSERT_MSG (nUeNodes % 3 == 0, "Total number of UEs must be divisible by 3 for slicing.");

  uint32_t numUePerSlice = nUeNodes / 3;

  NS_LOG_UNCOND ("Distributing " << nUeNodes << " UEs into 3 slices: " << numUePerSlice
                                 << " UEs per slice.");

  // Open output files for each UE's data rate report and initialize last values
  // We will now categorize files by slice type
  std::string sliceType;
  uint32_t sliceUeCounter = 0; // Counter for UEs within each slice type (0 to numUePerSlice-1)

  for (uint32_t u_idx = 0; u_idx < nUeNodes; ++u_idx) // u_idx is the index in ueNodes (0 to 11)
  {
      Ptr<Node> ueNode = ueNodes.Get (u_idx); // Get the actual Node* from the NodeContainer

      if (u_idx < numUePerSlice)
      {
          sliceType = "urllc";
          sliceUeCounter = u_idx;
      }
      else if (u_idx < 2 * numUePerSlice)
      {
          sliceType = "embb";
          sliceUeCounter = u_idx - numUePerSlice;
      }
      else
      {
          sliceType = "mmtc";
          sliceUeCounter = u_idx - (2 * numUePerSlice);
      }

      // Construct a user-friendly name for this UE, e.g., "urllc_ue_0"
      std::string ueSliceName = sliceType + "_ue_" + std::to_string (sliceUeCounter);
      g_ueIdToSliceName[ueNode->GetId ()] = ueSliceName; // Map actual Node ID to this name

      // Use this user-friendly name for the filename
      std::string filename = g_outputDir + "/" + ueSliceName + "_datarate.txt";
      g_ueDataRateFiles[ueNode->GetId ()] = new std::ofstream (filename.c_str (), std::ios_base::out | std::ios_base::trunc);
      if (!g_ueDataRateFiles[ueNode->GetId ()]->is_open ())
      {
          NS_LOG_ERROR ("Can't open file " << filename);
      }
      else
      {
          *g_ueDataRateFiles[ueNode->GetId ()] << "Time (s)\tThroughput (Mbps)" << std::endl;
      }
      g_ueLastTotalRxBytes[ueNode->GetId ()] = 0; // Initialize
      g_ueLastThroughputTime[ueNode->GetId ()] = 0; // Initialize
  }

  // URLLC Slice (UEs 0 to numUePerSlice-1 in ueNodes container)
  for (uint32_t u_idx = 0; u_idx < numUePerSlice; ++u_idx)
    {
      Ptr<Node> ueNode = ueNodes.Get (u_idx);
      PacketSinkHelper dlPacketSinkHelper ("ns3::UdpSocketFactory",
                                           InetSocketAddress (Ipv4Address::GetAny (), portUdp));
      ApplicationContainer sinkApps = dlPacketSinkHelper.Install (ueNode);
      g_uePacketSinks[ueNode->GetId ()] = StaticCast<PacketSink> (sinkApps.Get (0));
      ueSinkApp.Add (sinkApps);

      UdpClientHelper dlClient (ueIpIface.GetAddress (u_idx), portUdp);
      dlClient.SetAttribute ("Interval", TimeValue (MicroSeconds (400)));
      dlClient.SetAttribute ("MaxPackets", UintegerValue (UINT32_MAX));
      dlClient.SetAttribute ("PacketSize", UintegerValue (45));
      clientApp.Add (dlClient.Install (remoteHost));
      NS_LOG_UNCOND ("UE " << g_ueIdToSliceName[ueNode->GetId ()] << " (Node ID: " << ueNode->GetId() << ") assigned to URLLC slice.");
    }

  // eMBB Slice (UEs numUePerSlice to 2*numUePerSlice-1 in ueNodes container)
  for (uint32_t u_idx = numUePerSlice; u_idx < 2 * numUePerSlice; ++u_idx)
    {
      Ptr<Node> ueNode = ueNodes.Get (u_idx);
      PacketSinkHelper dlPacketSinkHelper ("ns3::UdpSocketFactory",
                                           InetSocketAddress (Ipv4Address::GetAny (), portUdp));
      ApplicationContainer sinkApps = dlPacketSinkHelper.Install (ueNode);
      g_uePacketSinks[ueNode->GetId ()] = StaticCast<PacketSink> (sinkApps.Get (0));
      ueSinkApp.Add (sinkApps);

      UdpClientHelper dlClient (ueIpIface.GetAddress (u_idx), portUdp);
      dlClient.SetAttribute ("Interval", TimeValue (MicroSeconds (3500)));
      dlClient.SetAttribute ("MaxPackets", UintegerValue (UINT32_MAX));
      dlClient.SetAttribute ("PacketSize", UintegerValue (4500));
      clientApp.Add (dlClient.Install (remoteHost));
      NS_LOG_UNCOND ("UE " << g_ueIdToSliceName[ueNode->GetId ()] << " (Node ID: " << ueNode->GetId() << ") assigned to eMBB slice.");
    }

  // mMTC Slice (UEs 2*numUePerSlice to nUeNodes-1 in ueNodes container)
  for (uint32_t u_idx = 2 * numUePerSlice; u_idx < nUeNodes; ++u_idx)
    {
      Ptr<Node> ueNode = ueNodes.Get (u_idx);
      PacketSinkHelper dlPacketSinkHelper ("ns3::UdpSocketFactory",
                                           InetSocketAddress (Ipv4Address::GetAny (), portUdp));
      ApplicationContainer sinkApps = dlPacketSinkHelper.Install (ueNode);
      g_uePacketSinks[ueNode->GetId ()] = StaticCast<PacketSink> (sinkApps.Get (0));
      ueSinkApp.Add (sinkApps);

      UdpClientHelper dlClient (ueIpIface.GetAddress (u_idx), portUdp);
      dlClient.SetAttribute ("Interval", TimeValue (MilliSeconds (80)));
      dlClient.SetAttribute ("MaxPackets", UintegerValue (UINT32_MAX));
      dlClient.SetAttribute ("PacketSize", UintegerValue (100));
      clientApp.Add (dlClient.Install (remoteHost));
      NS_LOG_UNCOND ("UE " << g_ueIdToSliceName[ueNode->GetId ()] << " (Node ID: " << ueNode->GetId() << ") assigned to mMTC slice.");
    }
  // --- End of Slicing Implementation ---


  // Start applications
  GlobalValue::GetValueByName ("simTime", doubleValue);
  double simTime = doubleValue.Get ();

  remoteHostSinkApp.Start (Seconds (0));
  ueSinkApp.Start (Seconds (0));

  clientApp.Start (MilliSeconds (100));
  clientApp.Stop (Seconds (simTime - 0.1));

  // Schedule periodic data rate reports
  Simulator::Schedule (Seconds (g_reportingInterval), &CalculateThroughput, std::cref(ueNodes), Seconds (g_reportingInterval));


  if (enableTraces)
    {
      mmwaveHelper->EnableTraces ();
    }

  // trick to enable PHY traces for the LTE stack
  Ptr<LteHelper> lteHelper = CreateObject<LteHelper> ();
  lteHelper->Initialize ();
  lteHelper->EnablePhyTraces ();
  lteHelper->EnableMacTraces ();

  // Since nodes are randomly allocated during each run we always need to print their positions
  PrintGnuplottableUeListToFile (g_outputDir + "/ues.txt");
  PrintGnuplottableEnbListToFile (g_outputDir + "/enbs.txt");

  bool run = true;
  if (run)
    {
      NS_LOG_UNCOND ("Simulation time is " << simTime << " seconds ");
      Simulator::Stop (Seconds (simTime));
      NS_LOG_INFO ("Run Simulation.");
      Simulator::Run ();
    }

  NS_LOG_INFO (lteHelper);
  Simulator::Destroy ();

  // Close all data rate files
  for (auto const& [ueId, outFilePtr] : g_ueDataRateFiles)
  {
      if (outFilePtr && outFilePtr->is_open ())
      {
          outFilePtr->close ();
          delete outFilePtr;
      }
  }
  g_ueDataRateFiles.clear ();

  NS_LOG_INFO ("Done.");
  return 0;
}

