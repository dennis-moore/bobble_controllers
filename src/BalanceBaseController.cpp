/******************************************************************************
 * Document this if it actually works
 * MMM
*******************************************************************************/

#include <sys/mman.h>

#include <bobble_controllers/BalanceBaseController.h>

namespace bobble_controllers {

    BalanceBaseController::~BalanceBaseController(void) {
	    run_thread_ = false;
	    subscriber_thread_->join();
    }

    void BalanceBaseController::init(ros::NodeHandle& nh)
    {
        state.ActiveControlMode = bobble_controllers::ControlModes::IDLE;
        reset();
        node_ = nh;
        loadConfig();
        setupFilters();
        setupControllers();
        pub_bobble_status_ = new realtime_tools::RealtimePublisher<bobble_controllers::BobbleBotStatus>(node_,
                                                    "bobble_balance_controller/bb_controller_status", 1);
        pub_joint_state_ = new realtime_tools::RealtimePublisher<sensor_msgs::JointState>(node_,
                                                                                         "/joint_states", 1);
        /// Initialize the message arrays for the left and right wheels
        pub_joint_state_->msg_.header.frame_id = "bobble_chassis_link";
        pub_joint_state_->msg_.name.push_back("right_wheel_hinge");
        pub_joint_state_->msg_.name.push_back("left_wheel_hinge");
        pub_joint_state_->msg_.position.push_back(0.0);
        pub_joint_state_->msg_.position.push_back(0.0);
        pub_joint_state_->msg_.velocity.push_back(0.0);
        pub_joint_state_->msg_.velocity.push_back(0.0);
        pub_joint_state_->msg_.effort.push_back(0.0);
        pub_joint_state_->msg_.effort.push_back(0.0);

	    run_thread_ = true;
        subscriber_thread_ = new std::thread(&BalanceBaseController::runSubscriber, this);
    }



    void BalanceBaseController::reset()
    {
        clearCommandState(processed_commands);
        clearCommandState(received_commands);
        clearCommandState(state.Cmds);
        state.ForwardVelocity = 0.0;
        state.DesiredTilt = 0.0;
        state.Tilt = 0.0;
        state.TiltDot = 0.0;
        state.Heading = 0.0;
        state.TurnRate = 0.0;
        state.LeftWheelVelocity = 0.0;
        state.RightWheelVelocity = 0.0;
        outputs.TiltEffort = 0.0;
        outputs.HeadingEffort = 0.0;
        outputs.LeftMotorEffortCmd = 0.0;
        outputs.RightMotorEffortCmd = 0.0;
    }

    void BalanceBaseController::update() {
        populateCommands();
        estimateState();
        applyFilters();
        runStateLogic();
        outputs.RightMotorEffortCmd = outputs.TiltEffort + outputs.HeadingEffort;
        outputs.LeftMotorEffortCmd = outputs.TiltEffort - outputs.HeadingEffort;
        applySafety();
        sendMotorCommands();
        write_controller_status_msg();
        write_joint_state_msg();
    }

    void BalanceBaseController::loadConfig() {
        unpackParameter("ControlLoopFrequency", config.ControlLoopFrequency, 500.0);
        unpackParameter("StartingTiltSafetyLimitDegrees", config.StartingTiltSafetyLimitDegrees, 4.0);
        unpackParameter("MaxTiltSafetyLimitDegrees", config.MaxTiltSafetyLimitDegrees, 20.0);
        unpackParameter("ControllerEffortMax", config.ControllerEffortMax, 0.4);
        unpackParameter("WheelVelocityAdjustment", config.WheelVelocityAdjustment, 0.0);
        unpackParameter("MadgwickFilterGain", config.MadgwickFilterGain, 0.01);
        unpackParameter("MeasuredTiltFilterFrequency", config.MeasuredTiltFilterFrequency, 0.0);
        unpackParameter("MeasuredTiltDotFilterFrequency", config.MeasuredTiltDotFilterFrequency, 0.0);
        unpackParameter("MeasuredHeadingFilterFrequency", config.MeasuredHeadingFilterFrequency, 0.0);
        unpackParameter("MeasuredTurnRateFilterFrequency", config.MeasuredTurnRateFilterFrequency, 0.0);
        unpackParameter("LeftWheelVelocityFilterFrequency", config.LeftWheelVelocityFilterFrequency, 0.0);
        unpackParameter("RightWheelVelocityFilterFrequency", config.RightWheelVelocityFilterFrequency, 0.0);
        unpackParameter("DesiredForwardVelocityFilterFrequency", config.DesiredForwardVelocityFilterFrequency, 0.0);
        unpackParameter("DesiredTurnRateFilterFrequency", config.DesiredTurnRateFilterFrequency, 0.0);
        unpackParameter("MaxVelocityCmd", config.MaxVelocityCmd, 0.5);
        unpackParameter("MaxTurnRateCmd", config.MaxTurnRateCmd, 0.5);
        unpackParameter("WheelRadiusMeters", config.WheelRadiusMeters, 0.05);
        unpackParameter("VelocityCmdScale", config.VelocityCmdScale, 1.0);
        unpackParameter("TurnCmdScale", config.TurnCmdScale, 1.0);
        unpackParameter("VelocityControlKp", config.VelocityControlKp, 1.0);
        unpackParameter("VelocityControlKd", config.VelocityControlKd, 0.1);
        unpackParameter("VelocityControlDerivFilter", config.VelocityControlDerivFilter, 50.0);
        unpackParameter("VelocityControlKi", config.VelocityControlKi, 0.01);
        unpackParameter("VelocityControlAlphaFilter", config.VelocityControlAlphaFilter, 0.05);
        unpackParameter("VelocityControlMaxIntegralOutput", config.VelocityControlMaxIntegralOutput, 0.6);
        unpackParameter("VelocityControlOutputLimitDegrees", config.VelocityControlOutputLimitDegrees, 5.0);
        unpackParameter("TiltControlKp", config.TiltControlKp, 1.0);
        unpackParameter("TiltControlKd", config.TiltControlKd, 0.01);
        unpackParameter("TiltControlAlphaFilter", config.TiltControlAlphaFilter, 0.05);
        unpackParameter("TiltOffset", config.TiltOffset, 0.0);
        unpackParameter("TiltDotOffset", config.TiltDotOffset, 0.0);
        unpackParameter("RollDotOffset", config.RollDotOffset, 0.0);
        unpackParameter("YawDotOffset", config.YawDotOffset, 0.0);
        unpackParameter("TurningControlKp", config.TurningControlKp, 1.0);
        unpackParameter("TurningControlKi", config.TurningControlKi, 0.01);
        unpackParameter("TurningControlKd", config.TurningControlKd, 0.01);
        unpackParameter("TurningControlDerivFilter", config.TurningControlDerivFilter, 50.0);
        unpackParameter("TurningOutputFilter", config.TurningOutputFilter, 0.0);
    }

    void BalanceBaseController::setupFilters() {
        filters.MeasuredTiltFilter.resetFilterParameters(1.0/config.ControlLoopFrequency, config.MeasuredTiltFilterFrequency, 1.0);
        filters.MeasuredTiltDotFilter.resetFilterParameters(1.0/config.ControlLoopFrequency, config.MeasuredTiltDotFilterFrequency, 1.0);
        filters.MeasuredHeadingFilter.resetFilterParameters(1.0/config.ControlLoopFrequency, config.MeasuredHeadingFilterFrequency, 1.0);
        filters.MeasuredTurnRateFilter.resetFilterParameters(1.0/config.ControlLoopFrequency, config.MeasuredTurnRateFilterFrequency, 1.0);
        filters.LeftWheelVelocityFilter.resetFilterParameters(1.0/config.ControlLoopFrequency, config.LeftWheelVelocityFilterFrequency, 1.0);
        filters.RightWheelVelocityFilter.resetFilterParameters(1.0/config.ControlLoopFrequency, config.RightWheelVelocityFilterFrequency, 1.0);
        filters.DesiredForwardVelocityFilter.resetFilterParameters(1.0/config.ControlLoopFrequency, config.DesiredForwardVelocityFilterFrequency, 1.0);
        filters.DesiredTurnRateFilter.resetFilterParameters(1.0/config.ControlLoopFrequency, config.DesiredTurnRateFilterFrequency, 1.0);
    }

    void BalanceBaseController::setupControllers() {
        // Setup velocity controller
        pid_controllers.VelocityControlPID.setPID(0.0, config.VelocityControlKp, config.VelocityControlKi, config.VelocityControlKd, config.VelocityControlDerivFilter, 1.0/config.ControlLoopFrequency);
        pid_controllers.VelocityControlPID.setOutputFilter(config.VelocityControlAlphaFilter);
        pid_controllers.VelocityControlPID.setMaxIOutput(config.VelocityControlMaxIntegralOutput);
        pid_controllers.VelocityControlPID.setOutputLimits(-config.VelocityControlOutputLimitDegrees * (M_PI / 180.0),
                                           config.VelocityControlOutputLimitDegrees * (M_PI / 180.0));
        // Setup tilt controller
        pid_controllers.TiltControlPID.setPID(0.0, config.TiltControlKp, 0.0, config.TiltControlKd, 0.0, 1.0/config.ControlLoopFrequency, PID_Direction::REVERSED);
        pid_controllers.TiltControlPID.setExternalDerivativeError(&state.TiltDot);
        pid_controllers.TiltControlPID.setOutputFilter(config.TiltControlAlphaFilter);
        pid_controllers.TiltControlPID.setOutputLimits(-config.ControllerEffortMax, config.ControllerEffortMax);
        pid_controllers.TiltControlPID.setSetpointRange(20.0 * (M_PI / 180.0));
        // Setup turning controller
        pid_controllers.TurningControlPID.setPID(0.0, config.TurningControlKp, config.TurningControlKi, config.TurningControlKd, config.TurningControlDerivFilter, 1.0/config.ControlLoopFrequency);
        pid_controllers.TurningControlPID.setExternalDerivativeError(&state.TurnRate);
        pid_controllers.TurningControlPID.setOutputFilter(config.TurningOutputFilter);
        pid_controllers.TurningControlPID.setMaxIOutput(1.0);
        pid_controllers.TurningControlPID.setOutputLimits(-config.ControllerEffortMax / 2.0, config.ControllerEffortMax / 2.0);
        pid_controllers.TurningControlPID.setSetpointRange(45.0 * (M_PI / 180.0));
    }

    void BalanceBaseController::runSubscriber() {
        ros::NodeHandle n;
        ros::Subscriber sub = n.subscribe("/bobble/bobble_balance_controller/bb_cmd", 1,
                                          &BalanceBaseController::subscriberCallBack, this);
        ros::Subscriber sub_cmd_vel = n.subscribe("/bobble/bobble_balance_controller/cmd_vel", 1,
                                          &BalanceBaseController::cmdVelCallback, this);
        ros::Rate loop_rate(20);
        while(ros::ok() && run_thread_)
        {
            control_command_mutex_.lock();
            processed_commands.StartupCmd = received_commands.StartupCmd;
            processed_commands.IdleCmd = received_commands.IdleCmd;
            processed_commands.DiagnosticCmd = received_commands.DiagnosticCmd;
            processed_commands.DesiredVelocity = received_commands.DesiredVelocity;
            processed_commands.DesiredHeading = received_commands.DesiredHeading;
            control_command_mutex_.unlock();

            loop_rate.sleep();
        }
        sub.shutdown();
        sub_cmd_vel.shutdown();
    }

    void BalanceBaseController::subscriberCallBack(const bobble_controllers::ControlCommands::ConstPtr &cmd) {
        received_commands.StartupCmd = cmd->StartupCmd;
        received_commands.IdleCmd = cmd->IdleCmd;
        received_commands.DiagnosticCmd = cmd->DiagnosticCmd;
    }

    void BalanceBaseController::cmdVelCallback(const geometry_msgs::Twist& command) {
        received_commands.DesiredVelocity = command.linear.x;
        received_commands.DesiredHeading = command.angular.z;
    }

    void BalanceBaseController::clearCommandState(bobble_controllers::BalanceControllerCommands& cmds)
    {
        cmds.StartupCmd = false;
        cmds.IdleCmd = false;
        cmds.DiagnosticCmd = false;
        cmds.DesiredVelocityRaw = 0.0;
        cmds.DesiredHeadingRaw = state.Heading;
        cmds.DesiredVelocity = 0.0;
        cmds.DesiredHeading = state.Heading;
    }

    void BalanceBaseController::populateCommands()
    {
        /// Load processed commands into the controller state.
        /// Lock mutex to prevent subscriber from writing to the controller
        /// state.
        control_command_mutex_.lock();
        state.Cmds.StartupCmd = processed_commands.StartupCmd;
        state.Cmds.IdleCmd = processed_commands.IdleCmd;
        state.Cmds.DiagnosticCmd = processed_commands.DiagnosticCmd;
        state.Cmds.DesiredVelocityRaw = processed_commands.DesiredVelocity * config.VelocityCmdScale;
        state.Cmds.DesiredHeadingRaw = processed_commands.DesiredHeading;
        control_command_mutex_.unlock();
    }

    void BalanceBaseController::applyFilters()
    {
        // Filters on IMU data to get a good orientation state estimate.
        state.Tilt = filters.MeasuredTiltFilter.filter(state.MeasuredTilt);
        state.TiltDot = filters.MeasuredTiltDotFilter.filter(state.MeasuredTiltDot);
        state.Heading = filters.MeasuredHeadingFilter.filter(state.WrappedMeasuredHeading);
        state.TurnRate = filters.MeasuredTurnRateFilter.filter(state.MeasuredTurnRate);
        // Filter wheel velocities and apply a wheel velocity adjustment in order to remove
        // a perceived wheel motion due to pendulum rotation
        state.LeftWheelVelocity = filters.LeftWheelVelocityFilter.filter(state.MeasuredLeftMotorVelocity) * config.WheelVelocityAdjustment;
        state.RightWheelVelocity = filters.RightWheelVelocityFilter.filter(state.MeasuredRightMotorVelocity) * config.WheelVelocityAdjustment;
        state.ForwardVelocity = config.WheelRadiusMeters*(state.RightWheelVelocity + state.LeftWheelVelocity)/2.0;
        state.Cmds.DesiredVelocity = filters.DesiredForwardVelocityFilter.filter(state.Cmds.DesiredVelocityRaw);
        state.Cmds.DesiredHeading = filters.DesiredTurnRateFilter.filter(state.Cmds.DesiredHeadingRaw);
        state.Cmds.DesiredVelocity = limit(state.Cmds.DesiredVelocity, config.MaxVelocityCmd);
        //state.Cmds.DesiredHeading = limit(state.Cmds.DesiredTurnRate, config.MaxTurnRateCmd);
    }

    void BalanceBaseController::applySafety()
    {
        // No effort when tilt angle is way out of whack.
        // You're going down. Don't fight it... just accept it.
        if (abs(state.Tilt) >= config.MaxTiltSafetyLimitDegrees * (M_PI / 180.0)) {
            outputs.RightMotorEffortCmd = 0.0;
            outputs.LeftMotorEffortCmd = 0.0;
        }
        outputs.RightMotorEffortCmd = limit(outputs.RightMotorEffortCmd, config.ControllerEffortMax);
        outputs.LeftMotorEffortCmd = limit(outputs.LeftMotorEffortCmd, config.ControllerEffortMax);
    }

    void BalanceBaseController::runStateLogic() {
        if (state.ActiveControlMode == bobble_controllers::ControlModes::IDLE) {
            idleMode();
        } else if (state.ActiveControlMode == bobble_controllers::ControlModes::DIAGNOSTIC) {
            diagnosticMode();
		} else if (state.ActiveControlMode == bobble_controllers::ControlModes::STARTUP) {
            startupMode();
        } else if (state.ActiveControlMode == bobble_controllers::ControlModes::BALANCE) {
            balanceMode();
        } else {
            outputs.TiltEffort = 0.0;
            outputs.HeadingEffort = 0.0;
        }
    }

    void BalanceBaseController::idleMode() {
        pid_controllers.VelocityControlPID.reset();
        pid_controllers.TiltControlPID.reset();
        pid_controllers.TurningControlPID.reset();
        state.DesiredTilt = 0.0;
        outputs.TiltEffort = 0.0;
        outputs.HeadingEffort = 0.0;
        if (state.Cmds.StartupCmd) {
            state.ActiveControlMode = bobble_controllers::ControlModes::STARTUP;
            reset();
            loadConfig();
            setupFilters();
            setupControllers();
        }
        if (state.Cmds.DiagnosticCmd) {
            state.ActiveControlMode = bobble_controllers::ControlModes::DIAGNOSTIC;
        }
    }

    void BalanceBaseController::diagnosticMode() {
        outputs.TiltEffort = state.Cmds.DesiredVelocity;
        outputs.HeadingEffort = state.Cmds.DesiredHeading;
        if (state.Cmds.IdleCmd) {
            state.ActiveControlMode = bobble_controllers::ControlModes::IDLE;
        }
    }

    void BalanceBaseController::startupMode() {
        // Wait until we're safe to proceed to balance mode
        if (abs(state.Tilt) >= config.StartingTiltSafetyLimitDegrees * (M_PI / 180.0)) {
            outputs.TiltEffort = 0.0;
            outputs.HeadingEffort = 0.0;
        } else {
            state.ActiveControlMode = bobble_controllers::ControlModes::BALANCE;
        }
    }

    void BalanceBaseController::balanceMode() {
        state.DesiredTilt = pid_controllers.VelocityControlPID.getOutput(state.Cmds.DesiredVelocity, state.ForwardVelocity);
        outputs.TiltEffort = pid_controllers.TiltControlPID.getOutput(state.DesiredTilt, state.Tilt);
        outputs.HeadingEffort = pid_controllers.TurningControlPID.getOutput(state.Cmds.DesiredHeading, state.Heading);
        if (state.Cmds.IdleCmd) {
            state.ActiveControlMode = bobble_controllers::ControlModes::IDLE;
        }
    }

    void BalanceBaseController::write_joint_state_msg() {
        if(pub_joint_state_->trylock())
        {
            pub_joint_state_->msg_.header.stamp = ros::Time::now();
            // Right Wheel
            pub_joint_state_->msg_.name[0] = "right_wheel_hinge";
            pub_joint_state_->msg_.effort[0] = outputs.RightMotorEffortCmd;
            pub_joint_state_->msg_.position[0] = state.MeasuredRightMotorPosition;
            pub_joint_state_->msg_.velocity[0] = state.MeasuredRightMotorVelocity;
            // Left Wheel
            pub_joint_state_->msg_.name[1] = "left_wheel_hinge";
            pub_joint_state_->msg_.effort[1] = outputs.LeftMotorEffortCmd;
            pub_joint_state_->msg_.position[1] = state.MeasuredLeftMotorPosition;
            pub_joint_state_->msg_.velocity[1] = state.MeasuredLeftMotorVelocity;
            pub_joint_state_->unlockAndPublish();
        }
    }

    void BalanceBaseController::write_controller_status_msg() {
        if(pub_bobble_status_->trylock()) {
            pub_bobble_status_->msg_.ControlMode = state.ActiveControlMode;
            pub_bobble_status_->msg_.MeasuredTiltDot = state.MeasuredTiltDot * (180.0 / M_PI);
            pub_bobble_status_->msg_.MeasuredTurnRate = state.MeasuredTurnRate * (180.0 / M_PI);
            pub_bobble_status_->msg_.FilteredTiltDot = state.FilteredTiltDot * (180.0 / M_PI);
            pub_bobble_status_->msg_.FilteredTurnRate = state.FilteredTurnRate * (180.0 / M_PI);
            pub_bobble_status_->msg_.Tilt = state.Tilt * (180.0 / M_PI);
            pub_bobble_status_->msg_.TiltRate = state.TiltDot * (180.0 / M_PI);
            pub_bobble_status_->msg_.Heading = state.Heading * (180.0 / M_PI);
            pub_bobble_status_->msg_.TurnRate = state.TurnRate * (180.0 / M_PI);
            pub_bobble_status_->msg_.ForwardVelocity = state.ForwardVelocity;
            pub_bobble_status_->msg_.DesiredVelocity = state.Cmds.DesiredVelocity;
	        pub_bobble_status_->msg_.DesiredTilt = state.DesiredTilt * (180.0 / M_PI);
            pub_bobble_status_->msg_.DesiredHeading = state.Cmds.DesiredHeading * (180.0 / M_PI);
            pub_bobble_status_->msg_.LeftMotorPosition = state.MeasuredLeftMotorPosition * (180.0 / M_PI);
            pub_bobble_status_->msg_.LeftMotorVelocity = state.MeasuredLeftMotorVelocity * (180.0 / M_PI);
            pub_bobble_status_->msg_.RightMotorPosition = state.MeasuredRightMotorPosition * (180.0 / M_PI);
            pub_bobble_status_->msg_.RightMotorVelocity = state.MeasuredRightMotorVelocity * (180.0 / M_PI);
            pub_bobble_status_->msg_.TiltEffort = outputs.TiltEffort;
            pub_bobble_status_->msg_.HeadingEffort = outputs.HeadingEffort;
            pub_bobble_status_->msg_.LeftMotorEffortCmd = outputs.LeftMotorEffortCmd;
            pub_bobble_status_->msg_.RightMotorEffortCmd = outputs.RightMotorEffortCmd;
            pub_bobble_status_->unlockAndPublish();
        }
    }

    void BalanceBaseController::unpackParameter(std::string parameterName, double &referenceToParameter,
                                                  double defaultValue) {
        if (!node_.getParam(parameterName, referenceToParameter)) {
            referenceToParameter = defaultValue;
            ROS_ERROR("%s not set for (namespace: %s) using %f.",
                      parameterName.c_str(),
                      node_.getNamespace().c_str(),
                      defaultValue);
        }
    }

    void BalanceBaseController::unpackParameter(std::string parameterName, std::string &referenceToParameter, std::string defaultValue)
    {
        if (!node_.getParam(parameterName, referenceToParameter)) {
            referenceToParameter = defaultValue;
            ROS_ERROR("%s not set for (namespace: %s) using %s.",
                      parameterName.c_str(),
                      node_.getNamespace().c_str(),
                      defaultValue.c_str());
        }
    }

    void BalanceBaseController::unpackFlag(std::string parameterName, bool &referenceToFlag,
                                             bool defaultValue) {
        if (!node_.getParam(parameterName, referenceToFlag)) {
            referenceToFlag = defaultValue;
            ROS_ERROR("%s not set for (namespace: %s). Setting to false.",
                      parameterName.c_str(),
                      node_.getNamespace().c_str());
        }
    }

    double BalanceBaseController::limit(double cmd, double max) {
        if (cmd < -max) {
            return -max;
        } else if (cmd > max) {
            return max;
        }
        return cmd;
    }

}
