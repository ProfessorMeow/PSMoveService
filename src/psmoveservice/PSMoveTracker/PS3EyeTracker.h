#ifndef PS3EYE_TRACKER_H
#define PS3EYE_TRACKER_H

#include "PSMoveDataFrame.h"
#include "PSMoveConfig.h"
#include "DeviceEnumerator.h"
#include "DeviceInterface.h"
#include <string>
#include <vector>
#include <deque>

class PS3EyeTrackerConfig : public PSMoveConfig
{
public:
    PS3EyeTrackerConfig(const std::string &fnamebase = "PS3EyeTrackerConfig")
    : PSMoveConfig(fnamebase)
    , is_valid(false)
    , max_poll_failure_count(100)
    {};
    
    virtual const boost::property_tree::ptree config2ptree();
    virtual void ptree2config(const boost::property_tree::ptree &pt);
    
    bool is_valid;
    long version;
    long max_poll_failure_count;

    static const int CONFIG_VERSION;
};

struct PS3EyeTrackerState : public CommonDeviceState
{   
    PS3EyeTrackerState()
    {
        clear();
    }
    
    void clear()
    {
        CommonDeviceState::clear();
        DeviceType = CommonDeviceState::PS3EYE;
    }
};

class PS3EyeTracker : public ITrackerInterface {
public:
    PS3EyeTracker();
    ~PS3EyeTracker();
        
    // PSMoveTracker
    bool open(); // Opens the first HID device for the controller
    
    // -- IDeviceInterface
    bool matchesDeviceEnumerator(const DeviceEnumerator *enumerator) const override;
    bool open(const DeviceEnumerator *enumerator) override;
    bool getIsOpen() const override;
    bool getIsReadyToPoll() const override;
    IDeviceInterface::ePollResult poll() override;
    void close() override;
    long getMaxPollFailureCount() const override;
    static CommonDeviceState::eDeviceType getDeviceTypeStatic()
    { return CommonDeviceState::PS3EYE; }
    CommonDeviceState::eDeviceType getDeviceType() const override;
    const CommonDeviceState *getState(int lookBack = 0) const override;
    
    // -- ITrackerInterface
    ITrackerInterface::eDriverType getDriverType() const override;
    std::string getUSBDevicePath() const override;
    bool getVideoFrameDimensions(int *out_width, int *out_height, int *out_stride) const override;
    const unsigned char *getVideoFrameBuffer() const override;

    // -- Getters
    inline const PS3EyeTrackerConfig &getConfig() const
    { return cfg; }

private:
    PS3EyeTrackerConfig cfg;
    std::string USBDevicePath;
    class PSEyeVideoCapture *VideoCapture;
    class PSEyeCaptureData *CaptureData;
    ITrackerInterface::eDriverType DriverType;
    
    // Read Controller State
    int NextPollSequenceNumber;
    std::deque<PS3EyeTrackerState> TrackerStates;
};
#endif // PS3EYE_TRACKER_H