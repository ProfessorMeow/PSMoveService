//-- includes -----
#include "ServerRequestHandler.h"

#include "BluetoothRequests.h"
#include "DeviceManager.h"
#include "DeviceEnumerator.h"
#include "OrientationFilter.h"
#include "PSMoveController.h"
#include "ServerControllerView.h"
#include "ServerDeviceView.h"
#include "ServerNetworkManager.h"
#include "PSMoveProtocol.pb.h"
#include "ServerLog.h"
#include "ServerUtility.h"
#include <cassert>
#include <bitset>
#include <map>
#include <boost/shared_ptr.hpp>

//-- pre-declarations -----
class ServerRequestHandlerImpl;
typedef boost::shared_ptr<ServerRequestHandlerImpl> ServerRequestHandlerImplPtr;

//-- definitions -----
struct RequestConnectionState
{
    int connection_id;
    std::bitset<ControllerManager::k_max_devices> active_controller_streams;
    AsyncBluetoothRequest *pending_bluetooth_request;
    ControllerStreamInfo active_controller_stream_info[ControllerManager::k_max_devices];

    RequestConnectionState()
        : connection_id(-1)
        , active_controller_streams()
        , pending_bluetooth_request(nullptr)
    {
        for (int index= 0; index < ControllerManager::k_max_devices; ++index)
        {
            active_controller_stream_info->Clear();
        }
    }
};
typedef boost::shared_ptr<RequestConnectionState> RequestConnectionStatePtr;
typedef std::map<int, RequestConnectionStatePtr> t_connection_state_map;
typedef std::map<int, RequestConnectionStatePtr>::iterator t_connection_state_iter;
typedef std::map<int, RequestConnectionStatePtr>::const_iterator t_connection_state_const_iter;
typedef std::pair<int, RequestConnectionStatePtr> t_id_connection_state_pair;

struct RequestContext
{
    RequestConnectionStatePtr connection_state;
    RequestPtr request;
};

//-- private implementation -----
class ServerRequestHandlerImpl
{
public:
    ServerRequestHandlerImpl(DeviceManager &deviceManager)
        : m_device_manager(deviceManager)
        , m_connection_state_map()
    {
    }

    virtual ~ServerRequestHandlerImpl()
    {
        // Without this we get a warning for deletion:
        // "Delete called on 'class ServerRequestHandlerImpl' that has virtual functions but non-virtual destructor"
    }

    bool any_active_bluetooth_requests() const
    {
        bool any_active= false;

        for (t_connection_state_const_iter iter= m_connection_state_map.begin(); iter != m_connection_state_map.end(); ++iter)
        {
            RequestConnectionStatePtr connection_state= iter->second;

            if (connection_state->pending_bluetooth_request != nullptr)
            {
                any_active= true;
                break;
            }
        }

        return any_active;
    }

    void update()
    {
        for (t_connection_state_iter iter= m_connection_state_map.begin(); iter != m_connection_state_map.end(); ++iter)
        {
            int connection_id= iter->first;
            RequestConnectionStatePtr connection_state= iter->second;

            // Update any asynchronous bluetooth requests
            if (connection_state->pending_bluetooth_request != nullptr)
            {
                bool delete_request;

                connection_state->pending_bluetooth_request->update();

                switch(connection_state->pending_bluetooth_request->getStatusCode())
                {
                case AsyncBluetoothRequest::running:
                    {
                        // Don't delete. Still have work to do
                        delete_request= false;
                    } break;
                case AsyncBluetoothRequest::succeeded:
                    {
                        SERVER_LOG_INFO("ServerRequestHandler") 
                            << "Async bluetooth request(" 
                            << connection_state->pending_bluetooth_request->getDescription() 
                            << ") completed.";
                        delete_request= true;
                    } break;
                case AsyncBluetoothRequest::failed:
                    {
                        SERVER_LOG_ERROR("ServerRequestHandler") 
                            << "Async bluetooth request(" 
                            << connection_state->pending_bluetooth_request->getDescription() 
                            << ") failed!";
                        delete_request= true;
                    } break;
                default:
                    assert(0 && "unreachable");
                }

                if (delete_request)
                {
                    delete connection_state->pending_bluetooth_request;
                    connection_state->pending_bluetooth_request= nullptr;
                }
            }
        }
    }

    ResponsePtr handle_request(int connection_id, RequestPtr request)
    {
        // The context holds everything a handler needs to evaluate a request
        RequestContext context;
        context.request= request;
        context.connection_state= FindOrCreateConnectionState(connection_id);

        // All responses track which request they came from
        PSMoveProtocol::Response *response= new PSMoveProtocol::Response;
        response->set_request_id(request->request_id());

        switch (request->type())
        {
            case PSMoveProtocol::Request_RequestType_GET_CONTROLLER_LIST:
                handle_request__get_controller_list(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_START_CONTROLLER_DATA_STREAM:
                handle_request__start_controller_data_stream(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_STOP_CONTROLLER_DATA_STREAM:
                handle_request__stop_controller_data_stream(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_SET_RUMBLE:
                handle_request__set_rumble(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_RESET_POSE:
                handle_request__reset_pose(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_UNPAIR_CONTROLLER:
                handle_request__unpair_controller(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_PAIR_CONTROLLER:
                handle_request__pair_controller(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_CANCEL_BLUETOOTH_REQUEST:
                handle_request__cancel_bluetooth_request(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_SET_LED_COLOR:
                handle_request__set_led_color(context, response);
                break;
            case PSMoveProtocol::Request_RequestType_SET_MAGNETOMETER_CALIBRATION:
                handle_request__set_magnetometer_calibration(context, response);
                break;
            default:
                assert(0 && "Whoops, bad request!");
        }

        return ResponsePtr(response);
    }

    void handle_client_connection_stopped(int connection_id)
    {
        t_connection_state_iter iter= m_connection_state_map.find(connection_id);

        if (iter != m_connection_state_map.end())
        {
            int connection_id= iter->first;
            RequestConnectionStatePtr connection_state= iter->second;

            // Cancel any pending asynchronous bluetooth requests
            if (connection_state->pending_bluetooth_request != nullptr)
            {
                assert(connection_state->pending_bluetooth_request->getStatusCode() == AsyncBluetoothRequest::running);

                connection_state->pending_bluetooth_request->cancel(AsyncBluetoothRequest::connectionClosed);

                delete connection_state->pending_bluetooth_request;
                connection_state->pending_bluetooth_request= nullptr;
            }

            // Remove the connection state from the state map
            m_connection_state_map.erase(iter);
        }
    }

    void publish_controller_data_frame(
         ServerControllerView *controller_view, 
         ServerRequestHandler::t_generate_controller_data_frame_for_stream callback)
    {
        int controller_id= controller_view->getDeviceID();

        // Notify any connections that care about the controller update
        for (t_connection_state_iter iter= m_connection_state_map.begin(); iter != m_connection_state_map.end(); ++iter)
        {
            int connection_id= iter->first;
            RequestConnectionStatePtr connection_state= iter->second;

            if (connection_state->active_controller_streams.test(controller_id))
            {
                const ControllerStreamInfo &streamInfo=
                    connection_state->active_controller_stream_info[controller_id];

                // Fill out a data frame specific to this stream using the given callback
                ControllerDataFramePtr data_frame(new PSMoveProtocol::ControllerDataFrame);
                callback(controller_view, &streamInfo, data_frame);

                // Send the controller data frame over the network
                ServerNetworkManager::get_instance()->send_controller_data_frame(connection_id, data_frame);
            }
        }
    }

protected:
    RequestConnectionStatePtr FindOrCreateConnectionState(int connection_id)
    {
        t_connection_state_iter iter= m_connection_state_map.find(connection_id);
        RequestConnectionStatePtr connection_state;

        if (iter == m_connection_state_map.end())
        {
            connection_state= RequestConnectionStatePtr(new RequestConnectionState());
            connection_state->connection_id= connection_id;

            m_connection_state_map.insert(t_id_connection_state_pair(connection_id, connection_state));
        }
        else
        {
            connection_state= iter->second;
        }

        return connection_state;
    }

    void handle_request__get_controller_list(
        const RequestContext &context, 
        PSMoveProtocol::Response *response)
    {
        PSMoveProtocol::Response_ResultControllerList* list= response->mutable_result_controller_list();

        for (int controller_id= 0; controller_id < m_device_manager.m_controller_manager.getMaxDevices(); ++controller_id)
        {
            ServerControllerViewPtr controller_view= m_device_manager.m_controller_manager.getControllerViewPtr(controller_id);

            if (controller_view->getIsOpen())
            {
                PSMoveProtocol::Response_ResultControllerList_ControllerInfo *controller_info= list->add_controllers();

                switch(controller_view->getControllerDeviceType())
                {
                case CommonControllerState::PSMove:
                    controller_info->set_controller_type(PSMoveProtocol::PSMOVE);
                    break;
                case CommonControllerState::PSNavi:
                    controller_info->set_controller_type(PSMoveProtocol::PSNAVI);
                    break;
                default:
                    assert(0 && "Unhandled controller type");
                }

                controller_info->set_controller_id(controller_id);
                controller_info->set_connection_type(
                    controller_view->getIsBluetooth()
                    ? PSMoveProtocol::Response_ResultControllerList_ControllerInfo_ConnectionType_BLUETOOTH
                    : PSMoveProtocol::Response_ResultControllerList_ControllerInfo_ConnectionType_USB);            
                controller_info->set_device_path(controller_view->getUSBDevicePath());
                controller_info->set_device_serial(controller_view->getSerial());
                controller_info->set_host_serial(controller_view->getHostBluetoothAddress());
            }
        }

        response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
    }

    void handle_request__start_controller_data_stream(
        const RequestContext &context, 
        PSMoveProtocol::Response *response)
    {
        const PSMoveProtocol::Request_RequestStartPSMoveDataStream& request=
            context.request->request_start_psmove_data_stream();
        int controller_id= request.controller_id();

        if (ServerUtility::is_index_valid(controller_id, m_device_manager.m_controller_manager.getMaxDevices()))
        {
            ControllerStreamInfo &streamInfo=
                context.connection_state->active_controller_stream_info[controller_id];

            // The controller manager will always publish updates regardless of who is listening.
            // All we have to do is keep track of which connections care about the updates.
            context.connection_state->active_controller_streams.set(controller_id, true);

            // Set control flags for the stream
            streamInfo.Clear();
            streamInfo.include_raw_sensor_data= request.include_raw_sensor_data();

            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
        }
        else
        {
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

    void handle_request__stop_controller_data_stream(
        const RequestContext &context,
        PSMoveProtocol::Response *response)
    {
        int controller_id= context.request->request_start_psmove_data_stream().controller_id();

        if (ServerUtility::is_index_valid(controller_id, m_device_manager.m_controller_manager.getMaxDevices()))
        {
            context.connection_state->active_controller_streams.set(controller_id, false);
            context.connection_state->active_controller_stream_info[controller_id].Clear();

            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
        }
        else
        {
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

    void handle_request__set_rumble(
        const RequestContext &context,
        PSMoveProtocol::Response *response)
    {
        const int controller_id= context.request->request_rumble().controller_id();
        const int rumble_amount= context.request->request_rumble().rumble();

        if (m_device_manager.m_controller_manager.setControllerRumble(controller_id, rumble_amount))
        {
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
        }
        else
        {
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

    void handle_request__reset_pose(
        const RequestContext &context, 
        PSMoveProtocol::Response *response)
    {
        const int controller_id= context.request->reset_pose().controller_id();

        if (m_device_manager.m_controller_manager.resetPose(controller_id))
        {
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
        }
        else
        {
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

    void handle_request__unpair_controller(
        const RequestContext &context, 
        PSMoveProtocol::Response *response)
    {
        const int connection_id= context.connection_state->connection_id;
        const int controller_id= context.request->unpair_controller().controller_id();        

        if (context.connection_state->pending_bluetooth_request == nullptr)
        {
            ServerControllerViewPtr controllerView= m_device_manager.getControllerViewPtr(controller_id);

            context.connection_state->pending_bluetooth_request =
                new AsyncBluetoothUnpairDeviceRequest(connection_id, controllerView);

            std::string description = context.connection_state->pending_bluetooth_request->getDescription();

            if (context.connection_state->pending_bluetooth_request->start())
            {
                SERVER_LOG_INFO("ServerRequestHandler") << "Async bluetooth request(" << description << ") started.";

                response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
            }
            else
            {
                SERVER_LOG_ERROR("ServerRequestHandler") << "Async bluetooth request(" << description << ") failed to start!";

                delete context.connection_state->pending_bluetooth_request;
                context.connection_state->pending_bluetooth_request = nullptr;

                response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
            }
        }
        else
        {
            SERVER_LOG_ERROR("ServerRequestHandler") 
                << "Can't start unpair request due to existing request: " 
                << context.connection_state->pending_bluetooth_request->getDescription();

            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

    void handle_request__pair_controller(
        const RequestContext &context, 
        PSMoveProtocol::Response *response)
    {
        const int connection_id= context.connection_state->connection_id;
        const int controller_id= context.request->pair_controller().controller_id();        

        if (context.connection_state->pending_bluetooth_request == nullptr)
        {
            ServerControllerViewPtr controllerView= m_device_manager.getControllerViewPtr(controller_id);

            context.connection_state->pending_bluetooth_request = 
                new AsyncBluetoothPairDeviceRequest(connection_id, controllerView);

            if (context.connection_state->pending_bluetooth_request->start())
            {
                SERVER_LOG_INFO("ServerRequestHandler") 
                    << "Async bluetooth request(" 
                    << context.connection_state->pending_bluetooth_request->getDescription() 
                    << ") started.";

                response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
            }
            else
            {
                SERVER_LOG_ERROR("ServerRequestHandler") 
                    << "Async bluetooth request(" 
                    << context.connection_state->pending_bluetooth_request->getDescription() 
                    << ") failed to start!";

                delete context.connection_state->pending_bluetooth_request;
                context.connection_state->pending_bluetooth_request= nullptr;

                response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
            }
        }
        else
        {
            SERVER_LOG_ERROR("ServerRequestHandler") 
                << "Can't start pair request due to existing request: " 
                << context.connection_state->pending_bluetooth_request->getDescription();

            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

    void handle_request__cancel_bluetooth_request(
        const RequestContext &context, 
        PSMoveProtocol::Response *response)
    {
        const int connection_id= context.connection_state->connection_id;
        const int controller_id= context.request->cancel_bluetooth_request().controller_id();        

        if (context.connection_state->pending_bluetooth_request != nullptr)
        {
            ServerControllerViewPtr controllerView= m_device_manager.getControllerViewPtr(controller_id);

            SERVER_LOG_INFO("ServerRequestHandler") 
                << "Async bluetooth request(" 
                << context.connection_state->pending_bluetooth_request->getDescription() 
                << ") Canceled.";

            context.connection_state->pending_bluetooth_request->cancel(AsyncBluetoothRequest::userRequested);
            
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
        }
        else
        {
            SERVER_LOG_ERROR("ServerRequestHandler") << "No active bluetooth operation active";

            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

    void handle_request__set_led_color(
        const RequestContext &context, 
        PSMoveProtocol::Response *response)
    {
        const int connection_id= context.connection_state->connection_id;
        const int controller_id= context.request->set_led_color_request().controller_id();
        const unsigned char r= ServerUtility::int32_to_int8_verify(context.request->set_led_color_request().r());
        const unsigned char g= ServerUtility::int32_to_int8_verify(context.request->set_led_color_request().g());
        const unsigned char b= ServerUtility::int32_to_int8_verify(context.request->set_led_color_request().b());

        ServerControllerViewPtr ControllerView= 
            m_device_manager.m_controller_manager.getControllerViewPtr(controller_id);

        if (ControllerView && ControllerView->getControllerDeviceType() == CommonDeviceState::PSMove)
        {
            PSMoveController *controller= ControllerView->castChecked<PSMoveController>();

            if (controller->setLED(r, g, b))
            {
                response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
            }
            else
            {
                response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
            }
        }
        else
        {
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

    void handle_request__set_magnetometer_calibration(
        const RequestContext &context, 
        PSMoveProtocol::Response *response)
    {
        const int controller_id= context.request->set_magnetometer_calibration_request().controller_id();

        ServerControllerViewPtr ControllerView= 
            m_device_manager.m_controller_manager.getControllerViewPtr(controller_id);

        if (ControllerView && ControllerView->getControllerDeviceType() == CommonDeviceState::PSMove)
        {
            const PSMoveProtocol::IntVector &magnetometer_min=
                context.request->set_magnetometer_calibration_request().magnetometer_min();
            const PSMoveProtocol::IntVector &magnetometer_max=
                context.request->set_magnetometer_calibration_request().magnetometer_max();
            const PSMoveProtocol::FloatVector &magnetometer_identity=
                context.request->set_magnetometer_calibration_request().magnetometer_identity();

            PSMoveController *controller= ControllerView->castChecked<PSMoveController>();
            PSMoveControllerConfig *config= controller->getConfigMutable();

            config->magnetometer_extents[0]= magnetometer_min.i();
            config->magnetometer_extents[1]= magnetometer_min.j();
            config->magnetometer_extents[2]= magnetometer_min.k();
            config->magnetometer_extents[3]= magnetometer_max.i();
            config->magnetometer_extents[4]= magnetometer_max.j();
            config->magnetometer_extents[5]= magnetometer_max.k();

            config->magnetometer_identity[0]= magnetometer_identity.i();
            config->magnetometer_identity[1]= magnetometer_identity.j();
            config->magnetometer_identity[2]= magnetometer_identity.k();

            config->save();

            // Reset the orientation filter state the calibration changed
            ControllerView->getOrientationFilter()->resetFilterState();

            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);
        }
        else
        {
            response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
        }
    }

private:
    DeviceManager &m_device_manager;
    t_connection_state_map m_connection_state_map;
};

//-- public interface -----
ServerRequestHandler *ServerRequestHandler::m_instance = NULL;

ServerRequestHandler::ServerRequestHandler(DeviceManager *deviceManager)
    : m_implementation_ptr(new ServerRequestHandlerImpl(*deviceManager))
{

}

ServerRequestHandler::~ServerRequestHandler()
{
    if (m_instance != NULL)
    {
        SERVER_LOG_ERROR("~ServerRequestHandler") << "Request handler deleted without calling shutdown first!";
    }

    delete m_implementation_ptr;
}

bool ServerRequestHandler::any_active_bluetooth_requests() const
{
    return m_implementation_ptr->any_active_bluetooth_requests();
}

bool ServerRequestHandler::startup()
{
    m_instance= this;
    return true;
}

void ServerRequestHandler::update()
{
    return m_implementation_ptr->update();
}

void ServerRequestHandler::shutdown()
{
    m_instance= NULL;
}

ResponsePtr ServerRequestHandler::handle_request(int connection_id, RequestPtr request)
{
    return m_implementation_ptr->handle_request(connection_id, request);
}

void ServerRequestHandler::handle_client_connection_stopped(int connection_id)
{
    return m_implementation_ptr->handle_client_connection_stopped(connection_id);
}

void ServerRequestHandler::publish_controller_data_frame(
    ServerControllerView *controller_view, 
    t_generate_controller_data_frame_for_stream callback)
{
    return m_implementation_ptr->publish_controller_data_frame(controller_view, callback);
}