///////////////////////////////////////////////////////////////////////////////
// Copyright © 2018 Deere & Company.  All Worldwide Rights Reserved.
// THIS MATERIAL IS THE PROPERTY OF DEERE & COMPANY. ALL USE, ALTERATIONS,
// DISCLOSURE, DISSEMINATION AND/OR REPRODUCTION NOT SPECIFICALLY AUTHORIZED
// BY DEERE & COMPANY IS PROHIBITED.
///////////////////////////////////////////////////////////////////////////////

#include "DocumentationTestHarness.h"
#include "PlatformTestSettings.h"
#include <Documentation/CANMessageHandlers/EinsteinCANMessageHandler.h>
#include <Documentation/CANMessageHandlers/MagmaCANMessageHandler.h>
#include <Documentation/DocumentationPlatformDetector.h>
#include <Documentation/Aggregator/TECUWheelSpeed.h>
#include <Documentation/Aggregator/GPSSpeed.h>
#include <Documentation/Calculator/GPSPresentation.h>
#include <Documentation/Calculator/FuelPerArea.h>
#include <Documentation/Calculator/TimeTillEmpty.h>
#include <Documentation/ExpiredTimerMock.h>
#include <J1939Messages/J1939FuelEconomy.h>
#include <J1939Messages/J1939EngineTemperature.h>
#include <J1939Messages/J1939VehicleHours.h>
#include <J1939Messages/J1939EngineFluidPressure.h>
#include <J1939Messages/J1939DashDisplay.h>
#include <J1939Messages/J1939ElectronicEngineController1.h>
#include <J1939Messages/J1939CruiseControlVehicleSpeed.h>
#include <J1939Messages/J1939VehicleFluids.h>
#include <JDProprietaryMessages/DisplayedEnginePower.h>
#include <Documentation/BasicDocServiceFake.h>
#include <SpeedProviderService/MockCSpeedProviderServiceProxy.h>
#include <PlatformModelConfigurationServiceMocks/PlatformModelConfigurationMock.h>
#include <Documentation/PlatformSpecificParameterHelper.h>
#include <HarvestUnitsMocks/GlobalHarvestUnitDependencyFactoryFake.h>

using namespace DocumentationService;
using namespace DocumentationInterfaceTypes;
using namespace DocumentationDataAccess;
using namespace UnitSystem;
using namespace DocTestHarness;
using namespace Documentation;
using namespace J1939Messages;
using namespace JDProprietaryMessages;
using namespace testing;
using namespace DocPlatformSpecificParameterHelper;

namespace
{
   const char* INSTANT_PREFIX = "Documentation.Instant";
   const double MAX_HOURS = 210554060.75;
   const double INVALID_HOURS = 210554061;
   const double HOURS_ABOVE_10000 = 11000.55;
   const double HOURS_BELOW_10000 = 9999.5678;
}

class DocumentationCombineAggregatorsIntegrationTest: public Test
   , public WithParamInterface<PlatformName>
{
public:
   DocumentationCombineAggregatorsIntegrationTest()
      : Params(GetParam())
      , DocumentationTestHarnessInstance(Params.PlatformType)
      , BasicDocumentationService(DocumentationTestHarnessInstance.GetBasicDocService())
      , InfoBusClient(DocumentationTestHarnessInstance.GetInfoBusValuesProvider())
      , CANHandler(NULL)
      , ModelConfigurationMockInstance(DocumentationTestHarnessInstance.GetModelConfiguration())
      , SelectorFactoryFake()
   {
      QVariantHash aggregatorsMap = PlatformSpecificParameterHelper::GetAggregators(Params.PlatformType);
      QVariantHash instantParametersMap = PlatformSpecificParameterHelper::GetInstantParameters(Params.PlatformType);
      QVariantHash calculators = PlatformSpecificParameterHelper::GetCalculators(Params.PlatformType);
      QVariantHash instantCalculators = PlatformSpecificParameterHelper::GetInstantCalculators(Params.PlatformType);
      QVariantHash counterParameters = PlatformSpecificParameterHelper::GetCounterParameters(Params.PlatformType);
      QVariantHash canParameters = PlatformSpecificParameterHelper::GetCANParameters(Params.PlatformType);

      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("Aggregators"))).WillRepeatedly(Return(aggregatorsMap));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("InstantParameters"))).WillRepeatedly(Return(instantParametersMap));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("Calculators"))).WillRepeatedly(Return(calculators));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("InstantCalculators"))).WillRepeatedly(Return(instantCalculators));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("CounterParameters"))).WillRepeatedly(Return(counterParameters));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("CANParameters"))).WillOnce(Return(canParameters));

      BasicDocumentationService->PersistenceIsReadyToUse("InstantValues");
      EXPECT_CALL(*(DocumentationTestHarnessInstance.GetExpiredTimer()), HasValueExpired(_, _)).WillRepeatedly(Return(true));
      EXPECT_CALL(*(DocumentationTestHarnessInstance.GetExpiredTimer()), HasValueExpired(_, _, _)).WillRepeatedly(Return(true));
   }

   void SetUp()
   {
      CANHandler = dynamic_cast<CombineCANMessageHandler*>(DocumentationTestHarnessInstance.GetCANMessageHandler());
      ASSERT_THAT(CANHandler, NotNull());
   }

   static void TearDownTestCase()
   {
      CleanupSingletonServices();
   }

   void VerifyResult(const JDDocumentationPresentationMessage& presentationMessage, const double& value,
         const EMeasureType& measureType, const std::string& unit, const bool& expectValid = false)
   {
      EMeasureType presentationUnit = static_cast<EMeasureType>(presentationMessage.presentationunit());

      JDNumber expectedValue(value, measureType);
      double variableNumberSourceValue = expectedValue.ConvertTo(presentationUnit);
      expectedValue.Set(variableNumberSourceValue, presentationUnit);

      EXPECT_NEAR(expectedValue, presentationMessage.presentationvalue(), GetTolerance(presentationMessage.decimalplaces()));
      EXPECT_EQ(expectValid, presentationMessage.isvalid());

      EXPECT_EQ(unit, presentationMessage.presentationunitstring());
   }

   void Initialize(const QString& parameter, const InfoBusValueProviderCallback& callback)
   {
      QString keyStr(QString("%1.%2").arg(INSTANT_PREFIX).arg(parameter));
      DocumentationTestHarnessInstance.SubscribeParameterOnInfoBus(keyStr.toStdString(), callback);
   }

   void InstantaneousFuelPerAreaInputProcess(const JDNumber& implementWidth, const JDNumber& fuelRate, const JDNumber& deltaDistance,  JDDocumentationPresentationMessage& presentationMessage, const bool& isRecordingOn = true)
   {
      int instantaneousFuelPerAreaKey;
      if(DocumentationPlatformDetector::EINSTEIN_COMBINE_PLATFORM == Params.PlatformType)
      {
         instantaneousFuelPerAreaKey = PlatformSpecificParameterHelper::GetEinsteinParameterKey("InstantCalculators", "InstantaneousFuelPerArea").toInt();
      }
      else if(DocumentationPlatformDetector::GRIZZLY_COMBINE_PLATFORM == Params.PlatformType)
      {
         instantaneousFuelPerAreaKey = PlatformSpecificParameterHelper::GetGrizzlyParameterKey("InstantCalculators", "InstantaneousFuelPerArea").toInt();
      }
      else
      {
         instantaneousFuelPerAreaKey = PlatformSpecificParameterHelper::GetMagmaParameterKey("InstantCalculators", "InstantaneousFuelPerArea").toInt();
      }
      FuelPerArea* fuelPerArea = dynamic_cast<FuelPerArea*>(BasicDocumentationService->GetInstantCalculatorPool(instantaneousFuelPerAreaKey));
      ASSERT_THAT(fuelPerArea, NotNull());

      fuelPerArea->UpdateWidth(implementWidth);
      fuelPerArea->UpdateFuelEconomy(fuelRate);
      fuelPerArea->UpdateRecordingSource(isRecordingOn);
      fuelPerArea->UpdateDeltaDistance(deltaDistance);
      fuelPerArea->UpdateCalculation(*(DocumentationTestHarnessInstance.GetExpiredTimer()));

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetInstantaneousFuelPerAreaData().payload());
   }

   void TimeTillEmptyInputProcess(const JDNumber& engineFuelTankCapacity, const JDNumber& fuelRate, const JDNumber& fuelTankLevel, const JDNumber& engineSpeed, JDDocumentationPresentationMessage& presentationMessage)
   {
      int timeTillEmptyKey;
      if(DocumentationPlatformDetector::EINSTEIN_COMBINE_PLATFORM == Params.PlatformType)
      {
         timeTillEmptyKey = PlatformSpecificParameterHelper::GetEinsteinParameterKey("InstantCalculators", "TimeToEmpty").toInt();
      }
      else if(DocumentationPlatformDetector::GRIZZLY_COMBINE_PLATFORM == Params.PlatformType)
      {
         timeTillEmptyKey = PlatformSpecificParameterHelper::GetGrizzlyParameterKey("InstantCalculators", "TimeToEmpty").toInt();
      }
      else
      {
         timeTillEmptyKey = PlatformSpecificParameterHelper::GetMagmaParameterKey("InstantCalculators", "TimeToEmpty").toInt();
      }
      TimeTillEmpty* timeTillEmpty = dynamic_cast<TimeTillEmpty*>(BasicDocumentationService->GetInstantCalculatorPool(timeTillEmptyKey));
      ASSERT_THAT(timeTillEmpty, NotNull());

      timeTillEmpty->UpdateEngineFuelTankCapacity(engineFuelTankCapacity);
      timeTillEmpty->UpdateEngineFuelRate(fuelRate);
      timeTillEmpty->UpdateEngineFuelTankLevel(fuelTankLevel);
      timeTillEmpty->UpdateEngineSpeed(engineSpeed);
      timeTillEmpty->UpdateCalculation(*(DocumentationTestHarnessInstance.GetExpiredTimer()));

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetTimeTillEmptyData().payload());
   }

   void EngineOilPressureInputProcess(const J1939EngineFluidPressure& message, JDDocumentationPresentationMessage& presentationMessage)
   {
      const unsigned char messageData[] = { 0xFF, 0xFF, 0xFF, 0x0D, 0xFF, 0xFF, 0xFF, 0xFF };
      Initialize("EngineOilPressure", &InfoBusValuesProviderFake::OnEngineOilPressure);
      JDCan::Message rawMessage(Params.BusType, messageData, 8, 0, 0, 0xffff, 0x28, 0x00, 0x01, true);

      QMetaObject::invokeMethod(CANHandler, "SetEngineOilPressure", Qt::DirectConnection,
            Q_ARG(J1939Messages::J1939EngineFluidPressure, message),
            Q_ARG(JDCan::Message, rawMessage));

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetEngineOilPressureData().payload());
   }

   void EngineFuelTankLevelInputProcess(const J1939DashDisplay& message, JDDocumentationPresentationMessage& presentationMessage)
   {
      const unsigned char messageData[] = { 0xFF, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
      Initialize("EngineFuelTankLevel", &InfoBusValuesProviderFake::OnEngineFuelTankLevel);
      JDCan::Message rawMessage(Params.BusType, messageData, 8, 0, 0, 0xffff, 0x28, 0x00, 0x01, true);

      QMetaObject::invokeMethod(CANHandler, "SetEngineFuelTankLevel", Qt::DirectConnection,
            Q_ARG(J1939Messages::J1939DashDisplay, message),
            Q_ARG(JDCan::Message, rawMessage));

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetEngineFuelTankLevelData().payload());
   }

   void EngineSpeedInputProcess(const J1939ElectronicEngineController1& message, JDDocumentationPresentationMessage& presentationMessage)
   {
      const unsigned char messageData[] = { 0xFF, 0xFF, 0xFF, 0x0D, 0x00, 0xFF, 0xE0, 0x25 };
      Initialize("EngineSpeed", &InfoBusValuesProviderFake::OnEngineSpeed);
      JDCan::Message rawMessage(Params.BusType, messageData, 8, 0, 0, 0xffff, 0x28, 0x00, 0x01, true);

      QMetaObject::invokeMethod(CANHandler, "SetEngineSpeed", Qt::DirectConnection,
            Q_ARG(J1939Messages::J1939ElectronicEngineController1, message),
            Q_ARG(JDCan::Message, rawMessage));

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetEngineSpeedData().payload());
   }

   void WheelSpeedInputProcess(const JDNumber& speed, JDDocumentationPresentationMessage& presentationMessage,
         SuggestedSpeedValuePresentationType speedValuePresentationType)
   {
      Initialize("WheelSpeed", &InfoBusValuesProviderFake::OnWheelSpeed);

      EXPECT_CALL(*DocumentationTestHarnessInstance.GetSpeedProviderServiceProxy(), GetWheelSpeed()).WillRepeatedly(ReturnRef(speed));

      int wheelSpeedKey;
      if(DocumentationPlatformDetector::EINSTEIN_COMBINE_PLATFORM == Params.PlatformType)
      {
         wheelSpeedKey = PlatformSpecificParameterHelper::GetEinsteinParameterKey("Aggregators", "WheelSpeed").toInt();
      }
      else if(DocumentationPlatformDetector::GRIZZLY_COMBINE_PLATFORM == Params.PlatformType)
      {
         wheelSpeedKey = PlatformSpecificParameterHelper::GetGrizzlyParameterKey("Aggregators", "WheelSpeed").toInt();
      }
      else
      {
         wheelSpeedKey = PlatformSpecificParameterHelper::GetMagmaParameterKey("Aggregators", "WheelSpeed").toInt();
      }
      TECUWheelSpeed* wheelSpeed = dynamic_cast<TECUWheelSpeed*>(BasicDocumentationService->GetAggregatorPool(wheelSpeedKey));

      ASSERT_THAT(wheelSpeed, NotNull());

      wheelSpeed->UpdateUPMSyncMessageWheelSpeed(speed);
      wheelSpeed->UpdateCreeperMode(speedValuePresentationType);
      wheelSpeed->NotifyConsumers(*DocumentationTestHarnessInstance.GetExpiredTimer());

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetWheelSpeedData().payload());
   }

   void GPSSpeedInputProcess(const JDNumber& speed, JDDocumentationPresentationMessage& presentationMessage,
         SuggestedSpeedValuePresentationType speedValuePresentationType)
   {
      KilometersPerHour_T wheelSpeedValue = 5;

      Initialize("GPSSpeed", &InfoBusValuesProviderFake::OnGPSSpeed);

      EXPECT_CALL(*DocumentationTestHarnessInstance.GetSpeedProviderServiceProxy(), GetGPSSpeed()).WillRepeatedly(ReturnRef(speed));

      int gpsSpeedKey;
      int gpsPresentationKey;
      if(DocumentationPlatformDetector::EINSTEIN_COMBINE_PLATFORM == Params.PlatformType)
      {
         gpsSpeedKey = PlatformSpecificParameterHelper::GetEinsteinParameterKey("Aggregators", "GPSSpeed").toInt();
         gpsPresentationKey = PlatformSpecificParameterHelper::GetEinsteinParameterKey("InstantCalculators", "GPSPresentation").toInt();
      }
      else if(DocumentationPlatformDetector::GRIZZLY_COMBINE_PLATFORM == Params.PlatformType)
      {
          gpsSpeedKey = PlatformSpecificParameterHelper::GetGrizzlyParameterKey("Aggregators", "GPSSpeed").toInt();
          gpsPresentationKey = PlatformSpecificParameterHelper::GetGrizzlyParameterKey("InstantCalculators", "GPSPresentation").toInt();
      }
      else
      {
         gpsSpeedKey = PlatformSpecificParameterHelper::GetMagmaParameterKey("Aggregators", "GPSSpeed").toInt();
         gpsPresentationKey = PlatformSpecificParameterHelper::GetMagmaParameterKey("InstantCalculators", "GPSPresentation").toInt();
      }
      GPSSpeed* gpsSpeed = dynamic_cast<GPSSpeed*>(BasicDocumentationService->GetAggregatorPool(gpsSpeedKey));

      ASSERT_THAT(gpsSpeed, NotNull());

      gpsSpeed->UpdateCreeperMode(speedValuePresentationType);
      gpsSpeed->NotifyConsumers(*DocumentationTestHarnessInstance.GetExpiredTimer());

      GPSPresentation* gpsPresentation = dynamic_cast<GPSPresentation*>(BasicDocumentationService->GetInstantCalculatorPool(gpsPresentationKey));
      ASSERT_THAT(gpsPresentation, NotNull());

      gpsPresentation->UpdateTECUWheelSpeed(SpeedInformationType(speedValuePresentationType, wheelSpeedValue, true));
      gpsPresentation->UpdateGPSSpeed(SpeedInformationType(speedValuePresentationType, speed, true));
      gpsPresentation->UpdateCalculation(*DocumentationTestHarnessInstance.GetExpiredTimer());

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetGPSSpeedData().payload());
   }

   void EngineCoolantTempratureInputProcess(const J1939EngineTemperature& msg, JDDocumentationPresentationMessage& presentationMessage)
   {
      const unsigned char messageData[] = { 0xFF, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
      Initialize("EngineCoolantTemperature", &InfoBusValuesProviderFake::OnEngineCoolantTemprature);
      JDCan::Message rawMessage(Params.BusType, messageData, 8, 0, 0, 0xffff, 0x28, 0x00, 0x01, true);

      QMetaObject::invokeMethod(CANHandler, "SetEngineTemperature", Qt::DirectConnection,
            Q_ARG(const J1939Messages::J1939EngineTemperature, msg),
            Q_ARG(const JDCan::Message, rawMessage));
      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetEngineCoolantTempratureData().payload());
   }

   void HydraulicOilTemperatureInputProcess(const J1939VehicleFluids& msg, JDDocumentationPresentationMessage& presentationMessage)
   {
      const unsigned char messageData[] = { 0xFE, 0x68, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
      Initialize("HydraulicOilTemperature", &InfoBusValuesProviderFake::OnHydraulicOilTemperature);
      JDCan::Message rawMessage(Params.BusType, messageData,  8, 0, 0, 0xffff, 0x28, 0x00, 0x1, true);

      QMetaObject::invokeMethod(CANHandler, "SetHydOilTemp", Qt::DirectConnection,
            Q_ARG(const J1939Messages::J1939VehicleFluids, msg),
            Q_ARG(const JDCan::Message, rawMessage));
      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetHydraulicOilTemperature().payload());
   }

   ~DocumentationCombineAggregatorsIntegrationTest() {}

protected:
   PlatformName Params;
   DocumentationTestHarness DocumentationTestHarnessInstance;
   BasicDocServiceFake* BasicDocumentationService;
   InfoBusValuesProviderFake* InfoBusClient;
   CombineCANMessageHandler* CANHandler;
   PMCService::ModelConfigurationMock* ModelConfigurationMockInstance;
   HarvestUnits::GlobalHarvestUnitDependencyFactoryFake SelectorFactoryFake;
};

class DocumentationCombineAggregatorsIntegrationEinsteinTest: public Test
   ,  public WithParamInterface<const char*>
{
public:
   DocumentationCombineAggregatorsIntegrationEinsteinTest()
      : Platform(GetParam())
      , DocumentationTestHarnessInstance(Platform)
      , BasicDocumentationService(DocumentationTestHarnessInstance.GetBasicDocService())
      , InfoBusClient(DocumentationTestHarnessInstance.GetInfoBusValuesProvider())
      , CANHandler(NULL)
      , ModelConfigurationMockInstance(DocumentationTestHarnessInstance.GetModelConfiguration())
      , SelectorFactoryFake()
   {
      QVariantHash aggregatorsMap = PlatformSpecificParameterHelper::GetAggregators(Platform);
      QVariantHash instantParametersMap = PlatformSpecificParameterHelper::GetInstantParameters(Platform);
      QVariantHash calculators = PlatformSpecificParameterHelper::GetCalculators(Platform);
      QVariantHash instantCalculators = PlatformSpecificParameterHelper::GetInstantCalulators(Platform);
      QVariantHash conuterParameters = PlatformSpecificParameterHelper::GetCounterParameters(Platform);
      QVariantHash canParameters = PlatformSpecificParameterHelper::GetCANParameters(Platform);

      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("Aggregators"))).WillRepeatedly(Return(aggregatorsMap));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("InstantParameters"))).WillRepeatedly(Return(instantParametersMap));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("Calculators"))).WillRepeatedly(Return(calculators));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("InstantCalculators"))).WillRepeatedly(Return(instantCalculators));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("CounterParameters"))).WillRepeatedly(Return(conuterParameters));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("CANParameters"))).WillRepeatedly(Return(canParameters));

      BasicDocumentationService->PersistenceIsReadyToUse("InstantValues");
      EXPECT_CALL(*(DocumentationTestHarnessInstance.GetExpiredTimer()), HasValueExpired(_, _)).WillRepeatedly(Return(true));
      EXPECT_CALL(*(DocumentationTestHarnessInstance.GetExpiredTimer()), HasValueExpired(_, _, _)).WillRepeatedly(Return(true));
   }

   void SetUp()
   {
      CANHandler = dynamic_cast<EinsteinCANMessageHandler*>(DocumentationTestHarnessInstance.GetCANMessageHandler());
      ASSERT_THAT(CANHandler, NotNull());
   }

   static void TearDownTestCase()
   {
      CleanupSingletonServices();
   }

   void VerifyResult(const JDDocumentationPresentationMessage& presentationMessage, const double& value,
         const EMeasureType& measureType, const std::string& unit, const bool& expectValid = false)
   {
      EMeasureType presentationUnit = static_cast<EMeasureType>(presentationMessage.presentationunit());

      JDNumber expectedValue(value, measureType);
      double variableNumberSourceValue = expectedValue.ConvertTo(presentationUnit);
      expectedValue.Set(variableNumberSourceValue, presentationUnit);

      EXPECT_NEAR(expectedValue, presentationMessage.presentationvalue(), GetTolerance(presentationMessage.decimalplaces()));
      EXPECT_EQ(expectValid, presentationMessage.isvalid());

      EXPECT_EQ(unit, presentationMessage.presentationunitstring());
   }

   void Initialize(const QString& parameter, const InfoBusValueProviderCallback& callback)
   {
      QString keyStr(QString("%1.%2").arg(INSTANT_PREFIX).arg(parameter));
      DocumentationTestHarnessInstance.SubscribeParameterOnInfoBus(keyStr.toStdString(), callback);
   }

   void VehicleHoursInputProcess(const J1939VehicleHours& message,  JDDocumentationPresentationMessage& presentationMessage)
   {
      const unsigned char messageData[] = { 0x19, 0x02, 0x03, 0x11, 0xFF, 0xFF, 0xFF, 0xFF };
      JDCan::Message rawMessage(JDCan::CAN_BUS_VEHICLE, messageData, 8, 0, 0, 0xFEE7, 0x28, 0x00, 0x01, true);

      QMetaObject::invokeMethod(CANHandler, "SetVehicleHours", Qt::DirectConnection,
            Q_ARG(J1939Messages::J1939VehicleHours, message),
            Q_ARG(JDCan::Message, rawMessage));

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetVehicleHoursData().payload());
   }

   ~DocumentationCombineAggregatorsIntegrationEinsteinTest() {}

protected:
   const cahr* Platform;
   DocumentationTestHarness DocumentationTestHarnessInstance;
   BasicDocServiceFake* BasicDocumentationService;
   InfoBusValuesProviderFake* InfoBusClient;
   CombineCANMessageHandler* CANHandler;
   PMCService::ModelConfigurationMock* ModelConfigurationMockInstance;
   HarvestUnits::GlobalHarvestUnitDependencyFactoryFake SelectorFactoryFake;
};

INSTANTIATE_TEST_CASE_P(Platform, DocumentationCombineAggregatorsIntegrationEinsteinTest,
      Values(
            PlatformName(DocumentationPlatformDetector::EINSTEIN_COMBINE_PLATFORM, JDCan::CAN_BUS_VEHICLE),
            PlatformName(DocumentationPlatformDetector::GRIZZLY_COMBINE_PLATFORM, JDCan::CAN_BUS_VEHICLE)
            ));

class DocumentationCombineAggregatorsIntegrationMagmaTest: public Test
{
public:
   DocumentationCombineAggregatorsIntegrationMagmaTest()
      : DocumentationTestHarnessInstance(DocumentationPlatformDetector::MAGMA_COMBINE_PLATFORM)
      , BasicDocumentationService(DocumentationTestHarnessInstance.GetBasicDocService())
      , InfoBusClient(DocumentationTestHarnessInstance.GetInfoBusValuesProvider())
      , CANHandler(NULL)
      , ModelConfigurationMockInstance(DocumentationTestHarnessInstance.GetModelConfiguration())
      , SelectorFactoryFake()
   {
      QVariantHash aggregatorsMap = PlatformSpecificParameterHelper::GetAggregators(DocumentationPlatformDetector::MAGMA_COMBINE_PLATFORM);
      QVariantHash instantParametersMap = PlatformSpecificParameterHelper::GetInstantParameters(DocumentationPlatformDetector::MAGMA_COMBINE_PLATFORM);
      QVariantHash magmaCalculators = PlatformSpecificParameterHelper::GetMagmaCalculators();
      QVariantHash magmaInstantCalculators = PlatformSpecificParameterHelper::GetMagmaInstaneCalculators();
      QVariantHash magmaCounterParameters = PlatformSpecificParameterHelper::GetMagmaCounterParameters();
      QVariantHash canParameters = PlatformSpecificParameterHelper::GetMagmaCANParameters();

      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("Aggregators"))).WillRepeatedly(Return(aggregatorsMap));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("InstantParameters"))).WillRepeatedly(Return(instantParametersMap));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("Calculators"))).WillRepeatedly(Return(magmaCalculators));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("InstantCalculators"))).WillRepeatedly(Return(magmaInstantCalculators));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("CounterParameters"))).WillRepeatedly(Return(magmaCounterParameters));
      EXPECT_CALL(*ModelConfigurationMockInstance, GetParametersFromGroup(QString("CANParameters"))).WillRepeatedly(Return(canParameters));

      BasicDocumentationService->PersistenceIsReadyToUse("InstantValues");
      EXPECT_CALL(*(DocumentationTestHarnessInstance.GetExpiredTimer()), HasValueExpired(_, _)).WillRepeatedly(Return(true));
      EXPECT_CALL(*(DocumentationTestHarnessInstance.GetExpiredTimer()), HasValueExpired(_, _, _)).WillRepeatedly(Return(true));
   }

   void SetUp()
   {
      CANHandler = dynamic_cast<MagmaCANMessageHandler*>(DocumentationTestHarnessInstance.GetCANMessageHandler());
      ASSERT_THAT(CANHandler, NotNull());
   }

   static void TearDownTestCase()
   {
      CleanupSingletonServices();
   }

   void VerifyResult(const JDDocumentationPresentationMessage& presentationMessage, const double& value,
         const EMeasureType& measureType, const std::string& unit, const bool& expectValid = false)
   {
      EMeasureType presentationUnit = static_cast<EMeasureType>(presentationMessage.presentationunit());

      JDNumber expectedValue(value, measureType);
      double variableNumberSourceValue = expectedValue.ConvertTo(presentationUnit);
      expectedValue.Set(variableNumberSourceValue, presentationUnit);

      EXPECT_NEAR(expectedValue, presentationMessage.presentationvalue(), GetTolerance(presentationMessage.decimalplaces()));
      EXPECT_EQ(expectValid, presentationMessage.isvalid());

      EXPECT_EQ(unit, presentationMessage.presentationunitstring());
   }

   void Initialize(const QString& parameter, const InfoBusValueProviderCallback& callback)
   {
      QString keyStr(QString("%1.%2").arg(INSTANT_PREFIX).arg(parameter));
      DocumentationTestHarnessInstance.SubscribeParameterOnInfoBus(keyStr.toStdString(), callback);
   }

   void EngineHoursInputProcess(const J1939VehicleHours& message,  JDDocumentationPresentationMessage& presentationMessage)
   {
      const unsigned char messageData[] = { 0x19, 0x02, 0x03, 0x11, 0xFF, 0xFF, 0xFF, 0xFF };
      const JDCan::UniqueDeviceId deviceId = 0x01;
      JDCan::Message rawMessage(JDCan::CAN_BUS_IMPLEMENT, messageData, 8, 0, 0, 0xffff, 0x28, 0x00, deviceId, true);

      QMetaObject::invokeMethod(CANHandler, "SetVehicleHours", Qt::DirectConnection,
            Q_ARG(J1939Messages::J1939VehicleHours, message),
            Q_ARG(JDCan::Message, rawMessage));

      BasicDocumentationService->UpdateInfoBus();
      presentationMessage.ParseFromString(InfoBusClient->GetVehicleHoursData().payload());
   }

   ~DocumentationCombineAggregatorsIntegrationMagmaTest() {}

protected:
   DocumentationTestHarness DocumentationTestHarnessInstance;
   BasicDocServiceFake* BasicDocumentationService;
   InfoBusValuesProviderFake* InfoBusClient;
   CombineCANMessageHandler* CANHandler;
   PMCService::ModelConfigurationMock* ModelConfigurationMockInstance;
   HarvestUnits::GlobalHarvestUnitDependencyFactoryFake SelectorFactoryFake;
};

INSTANTIATE_TEST_CASE_P(PlatformTypes, DocumentationCombineAggregatorsIntegrationTest,
      Values(
            PlatformName(DocumentationPlatformDetector::EINSTEIN_COMBINE_PLATFORM, JDCan::CAN_BUS_VEHICLE),
            PlatformName(DocumentationPlatformDetector::MAGMA_COMBINE_PLATFORM, JDCan::CAN_BUS_IMPLEMENT)
            ));

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidInstantaneousFuelPerAreaIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   JDNumber implementWidth(5, MT_Meter);
   JDNumber fuelRate(15, MT_LiterPerHour);
   JDNumber deltaDistance(0.005, MT_Kilometer);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   Initialize("InstantaneousFuelPerArea", &InfoBusValuesProviderFake::OnInstantaneousFuelPerArea);

   InstantaneousFuelPerAreaInputProcess(implementWidth, fuelRate, deltaDistance, presentationMessage);

   VerifyResult(presentationMessage, 0.05, MT_GallonPerAcre, "gal/ac", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidInstantaneousFuelPerAreaIsRequestedAndRecordingIsOffNoValueIsPublishedOnInfoBus)
{
   bool isRecordingOn = false;
   JDNumber implementWidth(5, MT_Meter);
   JDNumber fuelRate(15, MT_LiterPerHour);
   JDNumber deltaDistance(10.0, MT_Kilometer);
   JDDocumentationPresentationMessage presentationMessage;

   Initialize("InstantaneousFuelPerArea", &InfoBusValuesProviderFake::OnInstantaneousFuelPerArea);

   InstantaneousFuelPerAreaInputProcess(implementWidth, fuelRate, deltaDistance, presentationMessage, isRecordingOn);

   VerifyResult(presentationMessage, 0, MT_GallonPerAcre, "gal/ac");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidImplementWidthIsSetInvalidInstantaneousFuelPerAreaIsPublishedOnInfoBus)
{
   JDNumber implementWidth(5, MT_None);
   JDNumber fuelRate(15, MT_LiterPerHour);
   JDNumber deltaDistance(10.0, MT_Kilometer);
   JDDocumentationPresentationMessage presentationMessage;

   Initialize("InstantaneousFuelPerArea", &InfoBusValuesProviderFake::OnInstantaneousFuelPerArea);

   InstantaneousFuelPerAreaInputProcess(implementWidth, fuelRate, deltaDistance, presentationMessage);

   VerifyResult(presentationMessage, 0, MT_GallonPerAcre, "gal/ac");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidTimeTillEmptyIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   JDNumber engineFuelTankCapacity(100, MT_Liter);
   JDNumber fuelRate(10, MT_LiterPerHour);
   JDNumber fuelTankLevel(40, MT_Percent);
   JDNumber engineSpeed(1000, MT_RotationsPerMinute);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   Initialize("TimeTillEmpty", &InfoBusValuesProviderFake::OnTimeTillEmpty);

   TimeTillEmptyInputProcess(engineFuelTankCapacity, fuelRate, fuelTankLevel, engineSpeed, presentationMessage);

   VerifyResult(presentationMessage, 4, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineSpeedIsSetInvalidTimeTillEmptyIsPublishedOnInfoBus)
{
   JDNumber engineFuelTankCapacity(100, MT_Liter);
   JDNumber fuelRate(10, MT_LiterPerHour);
   JDNumber fuelTankLevel(40, MT_Percent);
   JDNumber engineSpeed(1000, MT_None);
   JDDocumentationPresentationMessage presentationMessage;

   Initialize("TimeTillEmpty", &InfoBusValuesProviderFake::OnTimeTillEmpty);

   TimeTillEmptyInputProcess(engineFuelTankCapacity, fuelRate, fuelTankLevel, engineSpeed, presentationMessage);

   VerifyResult(presentationMessage, 0, MT_Hours, "h");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationEinsteinTest, WhenValidVehicleHoursIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(1000);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   VehicleHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 1000, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationEinsteinTest, WhenInvalidVehicleHoursBelowMinValueIsRequestedZeroValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(-10);
   JDDocumentationPresentationMessage presentationMessage;

   VehicleHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 0, MT_Hours, "h");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationEinsteinTest, WhenValidVehicleHoursMinValueIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(0);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   VehicleHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 0, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationEinsteinTest, WhenValidVehicleHoursMaxValueIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(MAX_HOURS);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   VehicleHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, MAX_HOURS, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationEinsteinTest, WhenInvalidVehicleHoursAboveMaxValueIsRequestedZeroValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(INVALID_HOURS);
   JDDocumentationPresentationMessage presentationMessage;

   VehicleHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 0, MT_Hours, "h");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationEinsteinTest, WhenValueOfVehicleHoursGreaterThan10000IsPublishedValueIsRoundedOff)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(HOURS_ABOVE_10000);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   VehicleHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 11001, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationEinsteinTest, WhenValueOfVehicleHoursSmallerThan10000IsPublishedValueIsRoundedOff)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(HOURS_BELOW_10000);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;
   VehicleHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 9999.5, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_F(DocumentationCombineAggregatorsIntegrationMagmaTest, WhenValidEngineHoursIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(1000);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   EngineHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 1000, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_F(DocumentationCombineAggregatorsIntegrationMagmaTest, WhenInvalidEngineHoursBelowMinValueIsRequestedZeroValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(-10);
   JDDocumentationPresentationMessage presentationMessage;

   EngineHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 0, MT_Hours, "h");
}

///////////////////////////////////////////////////////////////////////////////
TEST_F(DocumentationCombineAggregatorsIntegrationMagmaTest, WhenValidEngineHoursMinValueIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(0);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   EngineHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 0, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_F(DocumentationCombineAggregatorsIntegrationMagmaTest, WhenValidEngineHoursMaxValueIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(MAX_HOURS);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   EngineHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, MAX_HOURS, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_F(DocumentationCombineAggregatorsIntegrationMagmaTest, WhenInvalidEngineHoursAboveMaxValueIsRequestedZeroValueIsPublishedOnInfoBus)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(INVALID_HOURS);
   JDDocumentationPresentationMessage presentationMessage;

   EngineHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 0, MT_Hours, "h");
}

///////////////////////////////////////////////////////////////////////////////
TEST_F(DocumentationCombineAggregatorsIntegrationMagmaTest, WhenValueOfEngineHoursGreaterThan10000IsPublishedValueIsRoundedOff)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(HOURS_ABOVE_10000);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;

   EngineHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 11001, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_F(DocumentationCombineAggregatorsIntegrationMagmaTest, WhenValueOfEngineHoursSmallerThan10000IsPublishedValueIsRoundedOff)
{
   Initialize("VehicleHours", &InfoBusValuesProviderFake::OnVehicleHours);

   J1939VehicleHours message(HOURS_BELOW_10000);
   JDDocumentationPresentationMessage presentationMessage;
   bool expectValid = true;
   EngineHoursInputProcess(message, presentationMessage);

   VerifyResult(presentationMessage, 9999.5, MT_Hours, "h", expectValid);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineOilPressureIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   J1939EngineFluidPressure message(232);
   JDDocumentationPresentationMessage presentationMessage;

   EngineOilPressureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 232, MT_Kilopascal, "PSI", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineOilPressureIsRequestedWithMinValueCorrectValueIsPublishedOnInfoBus)
{
   J1939EngineFluidPressure message(0);
   JDDocumentationPresentationMessage presentationMessage;

   EngineOilPressureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Kilopascal, "PSI", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineOilPressureIsRequestedWithMaxValueCorrectValueIsPublishedOnInfoBus)
{
   J1939EngineFluidPressure message(1000);
   JDDocumentationPresentationMessage presentationMessage;

   EngineOilPressureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 1000, MT_Kilopascal, "PSI", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineOilPressureIsRequestedWithBelowMinValueZeroValueIsPublishedOnInfoBus)
{
   J1939EngineFluidPressure message(-1);
   JDDocumentationPresentationMessage presentationMessage;

   EngineOilPressureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Kilopascal, "PSI");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineOilPressureIsRequestedWithAboveMaxValueZeroValueIsPublishedOnInfoBus)
{
   J1939EngineFluidPressure message(1001);
   JDDocumentationPresentationMessage presentationMessage;

   EngineOilPressureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Kilopascal, "PSI");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineFuelTankLevelIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   J1939DashDisplay message(20);
   JDDocumentationPresentationMessage presentationMessage;

   EngineFuelTankLevelInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 20, MT_Percent, "%", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineFuelTankLevelIsRequestedWithMinValueCorrectValueIsPublishedOnInfoBus)
{
   J1939DashDisplay message(0);
   JDDocumentationPresentationMessage presentationMessage;

   EngineFuelTankLevelInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Percent, "%", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineFuelTankLevelIsRequestedWithMaxValueCorrectValueIsPublishedOnInfoBus)
{
   J1939DashDisplay message(100);
   JDDocumentationPresentationMessage presentationMessage;

   EngineFuelTankLevelInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 100, MT_Percent, "%", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineFuelTankLevelIsRequestedWithBelowMinValueZeroValueIsPublishedOnInfoBus)
{
   J1939DashDisplay message(-1);
   JDDocumentationPresentationMessage presentationMessage;

   EngineFuelTankLevelInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Percent, "%");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineFuelTankLevelIsRequestedWithAboveMaxValueZeroValueIsPublishedOnInfoBus)
{
   J1939DashDisplay message(110);
   JDDocumentationPresentationMessage presentationMessage;

   EngineFuelTankLevelInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Percent, "%");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineSpeedIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   J1939ElectronicEngineController1 message(5000.12);
   JDDocumentationPresentationMessage presentationMessage;

   EngineSpeedInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 5000.12, MT_RotationsPerMinute, "n/min", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineSpeedIsRequestedWithMinValueCorrectValueIsPublishedOnInfoBus)
{
   J1939ElectronicEngineController1 message(0);
   JDDocumentationPresentationMessage presentationMessage;

   EngineSpeedInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_RotationsPerMinute, "n/min", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineSpeedIsRequestedWithMaxValueCorrectValueIsPublishedOnInfoBus)
{
   J1939ElectronicEngineController1 message(8030);
   JDDocumentationPresentationMessage presentationMessage;

   EngineSpeedInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 8030, MT_RotationsPerMinute, "n/min", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineSpeedIsRequestedWithBelowMinValueZeroValueIsPublishedOnInfoBus)
{
   J1939ElectronicEngineController1 message(-1);
   JDDocumentationPresentationMessage presentationMessage;

   EngineSpeedInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_RotationsPerMinute, "n/min");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineSpeedIsRequestedWithAboveMaxValueZeroValueIsPublishedOnInfoBus)
{
   J1939ElectronicEngineController1 message(9000);
   JDDocumentationPresentationMessage presentationMessage;

   EngineSpeedInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_RotationsPerMinute, "n/min");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidWheelSpeedIsRequestedWithCreeperModeAsEuropeCorrectValueIsPublishedInFootPerHourOnInfoBus)
{
   JDNumber speed(12, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   WheelSpeedInputProcess(speed, presentationMessage, EUROPE);
   VerifyResult(presentationMessage, 7.5, MT_StatuteMilePerHour, "mi/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidWheelSpeedIsRequestedWithMinValueAndNoCreeperModeCorrectValueIsPublishedInFootPerHourOnInfoBus)
{
   JDNumber speed(0, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   WheelSpeedInputProcess(speed, presentationMessage, NONE);
   VerifyResult(presentationMessage, 0, MT_StatuteMilePerHour, "mi/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidWheelSpeedIsRequestedWithMinValueAndCreeperModeAsNorthAmericaCorrectValueIsPublishedInFootPerHourOnInfoBus)
{
   JDNumber speed(0, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   WheelSpeedInputProcess(speed, presentationMessage, NORTH_AMERICA);
   VerifyResult(presentationMessage, 0, MT_StatuteMilePerHour, "mi/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidWheelSpeedIsRequestedWithMaxValueAndCreeperModeAsNorthAmericaCorrectValueIsPublishedOnInfoBus)
{
   JDNumber speed(250.996, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   WheelSpeedInputProcess(speed, presentationMessage, NORTH_AMERICA);
   VerifyResult(presentationMessage, 155.96, MT_StatuteMilePerHour, "mi/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidWheelSpeedIsRequestedBelowMinValueAndCreeperModeAsNorthAmericaNegativeValueIsPublishedOnInfoBus)
{
   JDNumber speed(-1, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   WheelSpeedInputProcess(speed, presentationMessage, NORTH_AMERICA);
   VerifyResult(presentationMessage, -0.62, MT_StatuteMilePerHour, "mi/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidWheelSpeedIsRequestedBelowMinValueAndCreeperModeAsEuropeNegativeValueIsPublishedOnInfoBus)
{
   JDNumber speed(-1, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   WheelSpeedInputProcess(speed, presentationMessage, EUROPE);
   VerifyResult(presentationMessage, -3281, MT_FootPerHour, "ft/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidWheelSpeedIsRequestedAboveMaxValueValueAndCreeperModeAsNorthAmericaIsPublishedOnInfoBus)
{
   JDNumber speed(300, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   WheelSpeedInputProcess(speed, presentationMessage, NORTH_AMERICA);
   VerifyResult(presentationMessage, 186.40, MT_StatuteMilePerHour, "mi/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidWheelSpeedUnitWithEuropeAsCreeperModeIsRequestedZeroValueIsPublishedOnInfoBus)
{
   JDNumber speed(5, MT_None);
   JDDocumentationPresentationMessage presentationMessage;

   WheelSpeedInputProcess(speed, presentationMessage, EUROPE);
   VerifyResult(presentationMessage, 0, MT_StatuteMilePerHour, "mi/h");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidGPSSpeedIsRequestedWithNoCreeperModeCorrectValueIsPublishedOnInfoBus)
{
   JDNumber speed(90, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   GPSSpeedInputProcess(speed, presentationMessage, NONE);
   VerifyResult(presentationMessage, 55.92, MT_StatuteMilePerHour, "mi/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidGPSSpeedIsRequestedWithCreeperModeAsEuropeNegativeValueIsPublishedInFootPerHourOnInfoBus)
{
   JDNumber speed(-1, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   GPSSpeedInputProcess(speed, presentationMessage, EUROPE);
   VerifyResult(presentationMessage, -3280.83, MT_FootPerHour, "ft/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidGPSSpeedIsRequestedWithCreeperModeAsNorthAmericaNegativeValueIsPublishedInFootPerHourOnInfoBus)
{
   JDNumber speed(-1, MT_KilometerPerHour);
   JDDocumentationPresentationMessage presentationMessage;

   GPSSpeedInputProcess(speed, presentationMessage, NORTH_AMERICA);
   VerifyResult(presentationMessage, -0.62, MT_StatuteMilePerHour, "mi/h", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineCoolantTempratureIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   J1939EngineTemperature message(30, 200);
   JDDocumentationPresentationMessage presentationMessage;

   EngineCoolantTempratureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 86, MT_Fahrenheit, "\xC2\xB0" "F", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineCoolantTempratureIsRequestedWithMinValueCorrectValueIsPublishedOnInfoBus)
{
   J1939EngineTemperature message(-40, 200);
   JDDocumentationPresentationMessage presentationMessage;

   EngineCoolantTempratureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, -40, MT_Fahrenheit, "\xC2\xB0" "F", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidEngineCoolantTempratureIsRequestedWithMaxValueCorrectValueIsPublishedOnInfoBus)
{
   J1939EngineTemperature message(210, 200);
   JDDocumentationPresentationMessage presentationMessage;

   EngineCoolantTempratureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 410, MT_Fahrenheit, "\xC2\xB0" "F", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineCoolantTempratureIsRequestedWithBelowMinValueZeroValueIsPublishedOnInfoBus)
{
   J1939EngineTemperature message(-150, 200);
   JDDocumentationPresentationMessage presentationMessage;

   EngineCoolantTempratureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Fahrenheit, "\xC2\xB0" "F");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidEngineCoolantTempratureIsRequestedWithAboveMaxValueZeroValueIsPublishedOnInfoBus)
{
   J1939EngineTemperature message(215, 200);
   JDDocumentationPresentationMessage presentationMessage;

   EngineCoolantTempratureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Fahrenheit, "\xC2\xB0" "F");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidHydraulicOilTemperatureIsRequestedCorrectValueIsPublishedOnInfoBus)
{
   J1939VehicleFluids message(30);
   JDDocumentationPresentationMessage presentationMessage;

   HydraulicOilTemperatureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 86, MT_Fahrenheit, "\xC2\xB0" "F", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidHydraulicOilTemperatureIsRequestedWithMinValueCorrectValueIsPublishedOnInfoBus)
{
   J1939VehicleFluids message(-40);
   JDDocumentationPresentationMessage presentationMessage;

   HydraulicOilTemperatureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, -40, MT_Fahrenheit, "\xC2\xB0" "F", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenValidHydraulicOilTemperatureIsRequestedWithMaxValueCorrectValueIsPublishedOnInfoBus)
{
   J1939VehicleFluids message(210);
   JDDocumentationPresentationMessage presentationMessage;

   HydraulicOilTemperatureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 410, MT_Fahrenheit, "\xC2\xB0" "F", true);
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidHydraulicOilTemperatureIsRequestedWithBelowMinValueZeroValueIsPublishedOnInfoBus)
{
   J1939VehicleFluids message(-150);
   JDDocumentationPresentationMessage presentationMessage;

   HydraulicOilTemperatureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Fahrenheit, "\xC2\xB0" "F");
}

///////////////////////////////////////////////////////////////////////////////
TEST_P(DocumentationCombineAggregatorsIntegrationTest, WhenInvalidHydraulicOilTemperatureIsRequestedWithAboveMaxValueZeroValueIsPublishedOnInfoBus)
{
   J1939VehicleFluids message(215);
   JDDocumentationPresentationMessage presentationMessage;

   HydraulicOilTemperatureInputProcess(message, presentationMessage);
   VerifyResult(presentationMessage, 0, MT_Fahrenheit, "\xC2\xB0" "F");
}

