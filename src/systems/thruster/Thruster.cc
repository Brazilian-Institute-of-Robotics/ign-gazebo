/*
 * Copyright (C) 2021 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <memory>
#include <mutex>
#include <string>

#include <ignition/msgs/double.pb.h>

#include <ignition/math/Helpers.hh>

#include <ignition/plugin/Register.hh>

#include <ignition/transport/Node.hh>

#include "ignition/gazebo/components/AngularVelocity.hh"
#include "ignition/gazebo/components/ChildLinkName.hh"
#include "ignition/gazebo/components/LinearVelocity.hh"
#include "ignition/gazebo/components/JointAxis.hh"
#include "ignition/gazebo/components/Pose.hh"
#include "ignition/gazebo/components/World.hh"
#include "ignition/gazebo/Link.hh"
#include "ignition/gazebo/Model.hh"
#include "ignition/gazebo/Util.hh"

#include "Thruster.hh"

using namespace ignition;
using namespace gazebo;
using namespace systems;

class ignition::gazebo::systems::ThrusterPrivateData
{
  /// \brief Mutex for read/write access to class
  public: std::mutex mtx;

  /// \brief Thrust output by propeller in N
  public: double thrust = 0.0;

  /// \brief The link entity which will spin
  public: ignition::gazebo::Entity linkEntity;

  /// \brief Axis along which the propeller spins
  public: ignition::math::Vector3d jointAxis;

  /// \brief ignition node for handling transport
  public: ignition::transport::Node node;

  /// \brief The PID which controls the rpm
  public: ignition::math::PID rpmController;

  /// \brief Maximum input force [N], default: 1000N
  /// TODO(chapulina) Make it configurable from SDF.
  public: double cmdMax = 1000;

  /// \brief Minimum input force [N], default: 1000N
  /// TODO(chapulina) Make it configurable from SDF.
  public: double cmdMin = -1000;

  /// \brief Thrust coefficient relating the propeller angular velocity to the
  /// thrust
  public: double thrustCoefficient = 1;

  /// \brief Density of fluid in kgm^-3, default: 1000kgm^-3
  public: double fluidDensity = 1000;

  /// \brief Diameter of propeller in m, default: 0.02
  public: double propellerDiameter = 0.02;

  /// \brief callback for handling thrust update
  public: void OnCmdThrust(const ignition::msgs::Double &_msg);

  /// \brief function which computes rpm from thrust
  /// \param[in] _thrust Thrust in N
  /// \return Angular velocity in rad/s
  public: double ThrustToAngularVec(double _thrust);
};

/////////////////////////////////////////////////
Thruster::Thruster()
{
  this->dataPtr = std::make_unique<ThrusterPrivateData>();
}

/////////////////////////////////////////////////
Thruster::~Thruster()
{
}

/////////////////////////////////////////////////
void Thruster::Configure(
  const ignition::gazebo::Entity &_entity,
  const std::shared_ptr<const sdf::Element> &_sdf,
  ignition::gazebo::EntityComponentManager &_ecm,
  ignition::gazebo::EventManager &/*_eventMgr*/)
{
  // Create model object, to access convenient functions
  auto model = ignition::gazebo::Model(_entity);
  auto modelName = model.Name(_ecm);

  // Get namespace
  std::string ns = modelName;
  if (_sdf->HasElement("namespace"))
  {
    ns = _sdf->Get<std::string>("namespace");
  }

  // Get joint name
  if (!_sdf->HasElement("joint_name"))
  {
    ignerr << "Missing <joint_name>. Plugin won't be initialized."
           << std::endl;
    return;
  }
  auto jointName = _sdf->Get<std::string>("joint_name");

  // Get thrust coefficient
  if (!_sdf->HasElement("thrust_coefficient"))
  {
    ignerr << "Missing <thrust_coefficient>. Plugin won't be initialized."
           << std::endl;
    return;
  }
  this->dataPtr->thrustCoefficient = _sdf->Get<double>("thrust_coefficient");

  // Get propeller diameter
  if (!_sdf->HasElement("propeller_diameter"))
  {
    ignerr << "Missing <propeller_diameter>. Plugin won't be initialized."
           << std::endl;
    return;
  }
  this->dataPtr->propellerDiameter = _sdf->Get<double>("propeller_diameter");

  // Get fluid density, default to water otherwise
  if (_sdf->HasElement("fluid_density"))
  {
    this->dataPtr->fluidDensity = _sdf->Get<double>("fluid_density");
  }
  igndbg << "Setting fluid density to: " << this->dataPtr->fluidDensity << "\n";

  auto jointEntity = model.JointByName(_ecm, jointName);
  if (kNullEntity == jointEntity)
  {
    ignerr << "Failed to find joint [" << jointName << "] in model ["
           << modelName << "]. Plugin not initialized." << std::endl;
    return;
  }

  this->dataPtr->jointAxis =
    _ecm.Component<ignition::gazebo::components::JointAxis>(jointEntity)
      ->Data().Xyz();

  // Keeping cmd_pos for backwards compatibility
  // TODO(chapulina) Deprecate cmd_pos, because the commands aren't positions
  std::string thrusterTopicOld = ignition::transport::TopicUtils::AsValidTopic(
    "/model/" + ns + "/joint/" + jointName + "/cmd_pos");

  this->dataPtr->node.Subscribe(
    thrusterTopicOld,
    &ThrusterPrivateData::OnCmdThrust,
    this->dataPtr.get());

  // Subscribe to force commands
  std::string thrusterTopic = ignition::transport::TopicUtils::AsValidTopic(
    "/model/" + ns + "/joint/" + jointName + "/cmd_thrust");

  this->dataPtr->node.Subscribe(
    thrusterTopic,
    &ThrusterPrivateData::OnCmdThrust,
    this->dataPtr.get());

  ignmsg << "Thruster listening to commands in [" << thrusterTopic << "]"
         << std::endl;

  // Get link entity
  auto childLink =
    _ecm.Component<ignition::gazebo::components::ChildLinkName>(jointEntity);
  this->dataPtr->linkEntity = model.LinkByName(_ecm, childLink->Data());

  // Create necessary components if not present.
  enableComponent<components::AngularVelocity>(_ecm, this->dataPtr->linkEntity);
  enableComponent<components::WorldAngularVelocity>(_ecm,
      this->dataPtr->linkEntity);

  double p         =  0.1;
  double i         =  0;
  double d         =  0;
  double iMax      =  1;
  double iMin      = -1;
  double cmdMax    = this->dataPtr->ThrustToAngularVec(this->dataPtr->cmdMax);
  double cmdMin    = this->dataPtr->ThrustToAngularVec(this->dataPtr->cmdMin);
  double cmdOffset =  0;

  if (_sdf->HasElement("p_gain"))
  {
    p = _sdf->Get<double>("p_gain");
  }
  if (!_sdf->HasElement("i_gain"))
  {
    i = _sdf->Get<double>("i_gain");
  }
  if (!_sdf->HasElement("d_gain"))
  {
    d = _sdf->Get<double>("d_gain");
  }

  this->dataPtr->rpmController.Init(
    p,
    i,
    d,
    iMax,
    iMin,
    cmdMax,
    cmdMin,
    cmdOffset);
}

/////////////////////////////////////////////////
void ThrusterPrivateData::OnCmdThrust(const ignition::msgs::Double &_msg)
{
  std::lock_guard<std::mutex> lock(mtx);
  this->thrust = ignition::math::clamp(ignition::math::fixnan(_msg.data()),
    this->cmdMin, this->cmdMax);
}

/////////////////////////////////////////////////
double ThrusterPrivateData::ThrustToAngularVec(double _thrust)
{
  // Thrust is proportional to the Rotation Rate squared
  // See Thor I Fossen's  "Guidance and Control of ocean vehicles" p. 246
  auto propAngularVelocity = sqrt(abs(
    _thrust /
      (this->fluidDensity
      * this->thrustCoefficient * pow(this->propellerDiameter, 4))));

  propAngularVelocity *= (_thrust > 0) ? 1: -1;

  return propAngularVelocity;
}

/////////////////////////////////////////////////
void Thruster::PreUpdate(
  const ignition::gazebo::UpdateInfo &_info,
  ignition::gazebo::EntityComponentManager &_ecm)
{
  if (_info.paused)
    return;

  ignition::gazebo::Link link(this->dataPtr->linkEntity);

  auto pose = worldPose(this->dataPtr->linkEntity, _ecm);

  // TODO(arjo129): add logic for custom coordinate frame
  auto unitVector = pose.Rot().RotateVector(
    this->dataPtr->jointAxis.Normalize());

  double desiredThrust;
  {
    std::lock_guard<std::mutex> lock(this->dataPtr->mtx);
    desiredThrust = this->dataPtr->thrust;
  }
  // Thrust is proportional to the Rotation Rate squared
  // See Thor I Fossen's  "Guidance and Control of ocean vehicles" p. 246
  auto desiredPropellerAngVel =
      this->dataPtr->ThrustToAngularVec(desiredThrust);
  auto currentAngular = (link.WorldAngularVelocity(_ecm))->Dot(unitVector);
  auto angularError = currentAngular - desiredPropellerAngVel;
  double torque = 0.0;
  if (abs(angularError) > 0.1)
    torque = this->dataPtr->rpmController.Update(angularError, _info.dt);

  link.AddWorldWrench(
    _ecm,
    unitVector * this->dataPtr->thrust,
    unitVector * torque);
}

IGNITION_ADD_PLUGIN(
  Thruster, System,
  Thruster::ISystemConfigure,
  Thruster::ISystemPreUpdate)

IGNITION_ADD_PLUGIN_ALIAS(Thruster, "ignition::gazebo::systems::Thruster")

