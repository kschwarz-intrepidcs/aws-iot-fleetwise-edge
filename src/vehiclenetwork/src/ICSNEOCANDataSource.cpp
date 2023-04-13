/**
 * Copyright 2020 Amazon.com, Inc. and its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: LicenseRef-.amazon.com.-AmznSL-1.0
 * Licensed under the Amazon Software License (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 * http://aws.amazon.com/asl/
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#if defined( IOTFLEETWISE_LINUX )
// Includes
#include "businterfaces/ICSNEOCANDataSource.h"
#include "ClockHandler.h"
#include "EnumUtility.h"
#include "LoggingModule.h"
#include "TraceModule.h"
#include <boost/lockfree/spsc_queue.hpp>
#include <cstring>
#include <iostream>

namespace Aws
{
namespace IoTFleetWise
{
namespace VehicleNetwork
{
using namespace Aws::IoTFleetWise::Platform::Utility;
static const std::string INTERFACE_NAME_KEY = "interfaceName";
static const std::string THREAD_IDLE_TIME_KEY = "threadIdleTimeMs";
static constexpr uint32_t MSB_MASK = 0X7FFFFFFFU;

ICSNEOCANDataSource::ICSNEOCANDataSource()
{
    mType = VehicleDataSourceType::CAN_SOURCE;
    mNetworkProtocol = VehicleDataSourceProtocol::RAW_SOCKET;
    mID = generateSourceID();
}

ICSNEOCANDataSource::~ICSNEOCANDataSource()
{
    // To make sure the thread stops during teardown of tests.
    if ( isAlive() )
    {
        stop();
    }
}

bool
ICSNEOCANDataSource::init( const std::vector<VehicleDataSourceConfig> &sourceConfigs )
{
    // Only one source config is supported on the CAN stack i.e. we manage one socket with
    // one single thread.
    if ( sourceConfigs.size() > 1 || sourceConfigs.empty() )
    {
        FWE_LOG_ERROR( "Only one source config is supported" );
        return false;
    }
    auto settingsIterator = sourceConfigs[0].transportProperties.find( std::string( INTERFACE_NAME_KEY ) );
    if ( settingsIterator == sourceConfigs[0].transportProperties.end() )
    {
        FWE_LOG_ERROR( "Could not find interfaceName in the config" );
        return false;
    }
    else
    {
        mIfName = settingsIterator->second;
    }

    mCircularBuffPtr =
        std::make_shared<VehicleMessageCircularBuffer>( sourceConfigs[0].maxNumberOfVehicleDataMessages );
    settingsIterator = sourceConfigs[0].transportProperties.find( std::string( THREAD_IDLE_TIME_KEY ) );
    if ( settingsIterator == sourceConfigs[0].transportProperties.end() )
    {
        FWE_LOG_ERROR( "Could not find threadIdleTimeMs in the config" );
        return false;
    }
    else
    {
        try
        {
            mIdleTimeMs = static_cast<uint32_t>( std::stoul( settingsIterator->second ) );
        }
        catch ( const std::exception &e )
        {
            FWE_LOG_ERROR( "Could not cast the threadIdleTimeMs, invalid input: " + std::string( e.what() ) );
            return false;
        }
    }

    mTimer.reset();
    return true;
}

bool
ICSNEOCANDataSource::start()
{
    // Prevent concurrent stop/init
    std::lock_guard<std::mutex> lock( mThreadMutex );
    // On multi core systems the shared variable mShouldStop must be updated for
    // all cores before starting the thread otherwise thread will directly end
    mShouldStop.store( false );
    // Make sure the thread goes into sleep immediately to wait for
    // the manifest to be available
    mShouldSleep.store( true );
    if ( !mThread.create( doWork, this ) )
    {
        FWE_LOG_TRACE( "CAN Data Source Thread failed to start" );
    }
    else
    {
        FWE_LOG_TRACE( "CAN Data Source Thread started" );
        mThread.setThreadName( "fwVNLinuxCAN" + std::to_string( mID ) );
    }
    return mThread.isActive() && mThread.isValid();
}

void
ICSNEOCANDataSource::suspendDataAcquisition()
{
    // Go back to sleep
    FWE_LOG_TRACE( "Going to sleep until a the resume signal. CAN Data Source: " + std::to_string( mID ) );
    mShouldSleep.store( true, std::memory_order_relaxed );
}

void
ICSNEOCANDataSource::resumeDataAcquisition()
{

    FWE_LOG_TRACE( "Resuming Network data acquisition on Data Source: " + std::to_string( mID ) );
    // Make sure the thread does not sleep anymore
    mResumeTime = mClock->systemTimeSinceEpochMs();
    mShouldSleep.store( false );
    // Wake up the worker thread.
    mWait.notify();
}

bool
ICSNEOCANDataSource::stop()
{
    std::lock_guard<std::mutex> lock( mThreadMutex );
    mShouldStop.store( true, std::memory_order_relaxed );
    mWait.notify();
    mThread.release();
    mShouldStop.store( false, std::memory_order_relaxed );
    FWE_LOG_TRACE( "CAN Data Source Thread stopped" );
    return !mThread.isActive();
}

bool
ICSNEOCANDataSource::shouldStop() const
{
    return mShouldStop.load( std::memory_order_relaxed );
}

bool
ICSNEOCANDataSource::shouldSleep() const
{
    return mShouldSleep.load( std::memory_order_relaxed );
}

void
ICSNEOCANDataSource::doWork( void *data )
{

    ICSNEOCANDataSource *dataSource = static_cast<ICSNEOCANDataSource *>( data );

    uint32_t activations = 0;
    bool wokeUpFromSleep =
        false; /**< This variable is true after the thread is woken up for example because a valid decoder manifest was
                  received until the thread sleeps for the next time when it is false again*/
    Timer logTimer;

    // Only enable polling once everything has been set up.
    if ( !dataSource->mDevice->enableMessagePolling() )
    {   
        FWE_LOG_ERROR( "Enabling message polling failed: " + icsneo::EventManager::GetInstance().getLastError().describe() );
        return;
    }
    
    do
    {
        activations++;
        if ( dataSource->shouldSleep() )
        {
            // We either just started or there was a decoder manifest update that we can't use
            // We should sleep
            FWE_LOG_TRACE( "No valid decoding dictionary available, Channel going to sleep " );
            dataSource->mWait.wait( Platform::Linux::Signal::WaitWithPredicate );
            wokeUpFromSleep = true;
        }

        dataSource->mTimer.reset();
        
        const auto &[messages, success] = dataSource->mDevice->getMessages();
        if ( !success )
        {            
            FWE_LOG_ERROR( "Unable to get messages: " + icsneo::EventManager::GetInstance().getLastError().describe() );
            break;
        }

        for ( const auto &message : messages )
        {
            if ( message->type == icsneo::Message::Type::Frame )
            {
                auto frame = std::static_pointer_cast<icsneo::Frame>( message );
                if ( frame->network.getType() == icsneo::Network::Type::CAN )
                {
                    auto canMessage = std::static_pointer_cast<icsneo::CANMessage>( message );
                    // Nanoseconds between Unix epoch and ICS epoch (2007-01-01T00:00:00Z)
                    static constexpr uint64_t timestampOffset = 1167609600000000000;
                    uint64_t milisecondTimestamp = (canMessage->timestamp + timestampOffset) / 1000000;
                    if ( !wokeUpFromSleep || milisecondTimestamp >= dataSource->mResumeTime )
                    {
                        dataSource->receivedMessages++;
                        VehicleDataMessage message;
                        message.setup( canMessage->arbid, canMessage->data, std::vector<boost::any>(), milisecondTimestamp );
                        if ( message.isValid() )
                        {
                            if ( !dataSource->mCircularBuffPtr->push( std::move( message ) ) )
                            {
                                dataSource->discardedMessages++;
                                TraceModule::get().setVariable( TraceVariable::DISCARDED_FRAMES,
                                                                dataSource->discardedMessages );
                                FWE_LOG_WARN( "Circular Buffer is full" );
                            }
                        }
                        else
                        {
                            FWE_LOG_WARN( "Message is not valid" );
                        }
                    }
                }
            }
        }
        if ( messages.size() <= 0 )
        {
            if ( logTimer.getElapsedMs().count() > static_cast<int64_t>( LoggingModule::LOG_AGGREGATION_TIME_MS ) )
            {
                // Nothing is in the ring buffer to consume. Go to idle mode for some time.
                FWE_LOG_TRACE(
                    "Activations: " + std::to_string( activations ) +
                        ". Waiting for some data to come. Idling for :" + std::to_string( dataSource->mIdleTimeMs ) +
                        " ms, processed " + std::to_string( dataSource->receivedMessages ) + " frames" );
                activations = 0;
                logTimer.reset();
            }
            dataSource->mWait.wait( static_cast<uint32_t>( dataSource->mIdleTimeMs ) );
            wokeUpFromSleep = false;
        }
    } while ( !dataSource->shouldStop() );
}

size_t
ICSNEOCANDataSource::queueSize() const
{
    return mCircularBuffPtr->read_available();
}

bool
ICSNEOCANDataSource::connect()
{
    auto devices = icsneo::FindAllDevices();

    if ( devices.empty() )
    {
        FWE_LOG_ERROR( "Failed to find device: " + icsneo::EventManager::GetInstance().getLastError().describe() );
        return false;
    }

    if ( devices.size() != 1 )
    {
        FWE_LOG_ERROR( "Expected one ICSNEO device, found " + std::to_string(devices.size()) );
        return false;
    }
    auto& device = devices.front();
    if ( !device->open() )
    {
        FWE_LOG_ERROR( "Unable to open device: " + icsneo::EventManager::GetInstance().getLastError().describe() );
        return false;
    }
    if ( !device->goOnline() )
    {
        FWE_LOG_ERROR( "Unable to go online: " + icsneo::EventManager::GetInstance().getLastError().describe() );
        return false;
    }

    mDevice = device;

    // Notify on connection success
    notifyListeners<const VehicleDataSourceID &>( &VehicleDataSourceListener::onVehicleDataSourceConnected, mID );
    // Start the main thread.
    return start();
}

bool
ICSNEOCANDataSource::disconnect()
{
    if ( !mDevice )
    {
        return false;
    }
    if ( !stop() && !mDevice->close() )
    {
        return false;
    }
    mDevice.reset();
    // Notify on connection closure
    notifyListeners<const VehicleDataSourceID &>( &VehicleDataSourceListener::onVehicleDataSourceDisconnected, mID );
    return true;
}

bool
ICSNEOCANDataSource::isAlive()
{
    if ( !mDevice || !mDevice->isOnline() || !mThread.isValid() || !mThread.isActive() )
    {
        return false;
    }
    return true;
}

} // namespace VehicleNetwork
} // namespace IoTFleetWise
} // namespace Aws
#endif // IOTFLEETWISE_LINUX
