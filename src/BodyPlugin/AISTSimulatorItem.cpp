/*!
  @file
  @author Shin'ichiro Nakaoka
*/

#include "AISTSimulatorItem.h"
#include "BodyMotionItem.h"
#include "ControllerItem.h"
#include <cnoid/ItemManager>
#include <cnoid/Archive>
#include <cnoid/EigenArchive>
#include <cnoid/DyWorld>
#include <cnoid/DyBody>
#include <cnoid/ForwardDynamicsCBM>
#include <cnoid/ConstraintForceSolver>
#include <cnoid/LeggedBodyHelper>
#include <cnoid/FloatingNumberString>
#include <cnoid/EigenUtil>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <fstream>
#include <iomanip>
#include "gettext.h"

using namespace std;
using namespace cnoid;

// for Windows
#undef min
#undef max

namespace {

const bool TRACE_FUNCTIONS = false;
const bool ENABLE_DEBUG_OUTPUT = false;
const double DEFAULT_GRAVITY_ACCELERATION = 9.80665;

class HighGainControllerItem : public ControllerItem
{
    BodyPtr body;
    MultiValueSeqPtr qseqRef;
    int currentFrame;
    int lastFrame;
    int numJoints;

public:
    HighGainControllerItem(BodyItem* bodyItem, BodyMotionItem* bodyMotionItem) {
        qseqRef = bodyMotionItem->jointPosSeq();
        setName(str(fmt(_("HighGain Controller with %1%")) % bodyMotionItem->name()));
    }

    virtual bool start(const Target& target) {
        body = target.body();
        currentFrame = 0;
        lastFrame = std::max(0, qseqRef->numFrames() - 1);
        numJoints = std::min(body->numJoints(), qseqRef->numParts());
        if(qseqRef->numFrames() == 0){
            putMessage(_("Reference motion is empty()."));
            return false;
        }
        if(fabs(qseqRef->frameRate() - (1.0 / target.worldTimeStep())) > 1.0e-6){
            putMessage(_("The frame rate of the reference motion is different from the world frame rate."));
            return false;
        }
        control();
        return true;
    }

    virtual double timeStep() const {
        return qseqRef->getTimeStep();
    }
        
    virtual void input() { }

    virtual bool control() {

        if(++currentFrame > lastFrame){
            currentFrame = lastFrame;
            return false;
        }
        return true;
    }
        
    virtual void output() {

        int prevFrame = std::max(currentFrame - 1, 0);
        int nextFrame = std::min(currentFrame + 1, lastFrame);
            
        MultiValueSeq::Frame q0 = qseqRef->frame(prevFrame);
        MultiValueSeq::Frame q1 = qseqRef->frame(currentFrame);
        MultiValueSeq::Frame q2 = qseqRef->frame(nextFrame);

        double dt = qseqRef->getTimeStep();
        double dt2 = dt * dt;

        for(int i=0; i < numJoints; ++i){
            Link* joint = body->joint(i);
            joint->q() = q1[i];
            joint->dq() = (q2[i] - q1[i]) / dt;
            joint->ddq() = (q2[i] - 2.0 * q1[i] + q0[i]) / dt2;
        }
    }
        
    virtual void stop() { }

};
}


namespace cnoid {
  
class AISTSimulatorItemImpl
{
public:
    AISTSimulatorItem* self;

    World<ConstraintForceSolver> world;
        
    Selection dynamicsMode;
    Selection integrationMode;
    Vector3 gravity;
    double staticFriction;
    double slipFriction;
    FloatingNumberString contactCullingDistance;
    FloatingNumberString contactCullingDepth;
    FloatingNumberString errorCriterion;
    int maxNumIterations;
    FloatingNumberString contactCorrectionDepth;
    FloatingNumberString contactCorrectionVelocityRatio;
    double epsilon;
    bool is2Dmode;
    bool isKinematicWalkingEnabled;

    typedef std::map<Body*, int> BodyIndexMap;
    BodyIndexMap bodyIndexMap;

    AISTSimulatorItemImpl(AISTSimulatorItem* self);
    AISTSimulatorItemImpl(AISTSimulatorItem* self, const AISTSimulatorItemImpl& org);
    bool initializeSimulation(const std::vector<SimulationBody*>& simBodies);
    void addBody(SimulationBody* simBody);
    void doPutProperties(PutPropertyFunction& putProperty);
    bool store(Archive& archive);
    bool restore(const Archive& archive);

    // for debug
    ofstream os;
};
}


void AISTSimulatorItem::initializeClass(ExtensionManager* ext)
{
    ext->itemManager().registerClass<AISTSimulatorItem>(N_("AISTSimulatorItem"));
    ext->itemManager().addCreationPanel<AISTSimulatorItem>();
}


AISTSimulatorItem::AISTSimulatorItem()
{
    impl = new AISTSimulatorItemImpl(this);
}


AISTSimulatorItemImpl::AISTSimulatorItemImpl(AISTSimulatorItem* self)
    : self(self),
      dynamicsMode(AISTSimulatorItem::N_DYNAMICS_MODES, CNOID_GETTEXT_DOMAIN_NAME),
      integrationMode(AISTSimulatorItem::N_INTEGRATION_MODES, CNOID_GETTEXT_DOMAIN_NAME)
{
    dynamicsMode.setSymbol(AISTSimulatorItem::FORWARD_DYNAMICS,  N_("Forward dynamics"));
    dynamicsMode.setSymbol(AISTSimulatorItem::HG_DYNAMICS,       N_("High-gain dynamics"));
    dynamicsMode.setSymbol(AISTSimulatorItem::KINEMATICS,        N_("Kinematics"));

    integrationMode.setSymbol(AISTSimulatorItem::EULER_INTEGRATION,  N_("Euler"));
    integrationMode.setSymbol(AISTSimulatorItem::RUNGE_KUTTA_INTEGRATION,  N_("Runge Kutta"));
    integrationMode.select(AISTSimulatorItem::RUNGE_KUTTA_INTEGRATION);
    
    gravity << 0.0, 0.0, -DEFAULT_GRAVITY_ACCELERATION;

    ConstraintForceSolver& cfs = world.constraintForceSolver;
    staticFriction = cfs.staticFriction();
    slipFriction = cfs.slipFriction();
    contactCullingDistance = cfs.contactCullingDistance();
    contactCullingDepth = cfs.contactCullingDepth();
    epsilon = cfs.coefficientOfRestitution();
    
    errorCriterion = cfs.gaussSeidelErrorCriterion();
    maxNumIterations = cfs.gaussSeidelMaxNumIterations();
    contactCorrectionDepth = cfs.contactCorrectionDepth();
    contactCorrectionVelocityRatio = cfs.contactCorrectionVelocityRatio();

    isKinematicWalkingEnabled = false;
    is2Dmode = false;
}


AISTSimulatorItem::AISTSimulatorItem(const AISTSimulatorItem& org)
    : SimulatorItem(org),
      impl(new AISTSimulatorItemImpl(this, *org.impl))
{

}


AISTSimulatorItemImpl::AISTSimulatorItemImpl(AISTSimulatorItem* self, const AISTSimulatorItemImpl& org)
    : self(self),
      dynamicsMode(org.dynamicsMode),
      integrationMode(org.integrationMode)
{
    gravity = org.gravity;
    staticFriction = org.staticFriction;
    slipFriction = org.slipFriction;
    contactCullingDistance = org.contactCullingDistance;
    contactCullingDepth = org.contactCullingDepth;
    errorCriterion = org.errorCriterion;
    maxNumIterations = org.maxNumIterations;
    contactCorrectionDepth = org.contactCorrectionDepth;
    contactCorrectionVelocityRatio = org.contactCorrectionVelocityRatio;
    epsilon = org.epsilon;
    isKinematicWalkingEnabled = org.isKinematicWalkingEnabled;
    is2Dmode = org.is2Dmode;
}


AISTSimulatorItem::~AISTSimulatorItem()
{
    delete impl;
}


void AISTSimulatorItem::setDynamicsMode(int mode)
{
    impl->dynamicsMode.select(mode);
}


void AISTSimulatorItem::setIntegrationMode(int mode)
{
    impl->integrationMode.select(mode);
}


void AISTSimulatorItem::setGravity(const Vector3& gravity)
{
    impl->gravity = gravity;
}


void AISTSimulatorItem::setStaticFriction(double value)
{
    impl->staticFriction = value; 
}


void AISTSimulatorItem::setSlipFriction(double value)
{
    impl->slipFriction = value;
}


void AISTSimulatorItem::setContactCullingDistance(double value)    
{
    impl->contactCullingDistance = value;
}


void AISTSimulatorItem::setContactCullingDepth(double value)    
{
    impl->contactCullingDepth = value;
}

    
void AISTSimulatorItem::setErrorCriterion(double value)    
{
    impl->errorCriterion = value;
}

    
void AISTSimulatorItem::setMaxNumIterations(int value)
{
    impl->maxNumIterations = value;   
}


void AISTSimulatorItem::setContactCorrectionDepth(double value)
{
    impl->contactCorrectionDepth = value;
}


void AISTSimulatorItem::setContactCorrectionVelocityRatio(double value)
{
    impl->contactCorrectionVelocityRatio = value;
}


void AISTSimulatorItem::setEpsilon(double epsilon)
{
    impl->epsilon = epsilon;
}


void AISTSimulatorItem::set2Dmode(bool on)
{
    impl->is2Dmode = on;
}


void AISTSimulatorItem::setKinematicWalkingEnabled(bool on)
{
    impl->isKinematicWalkingEnabled = on;
}


ItemPtr AISTSimulatorItem::doDuplicate() const
{
    return new AISTSimulatorItem(*this);
}


SimulationBodyPtr AISTSimulatorItem::createSimulationBody(BodyPtr orgBody)
{
    return new SimulationBody(new DyBody(*orgBody));
}


ControllerItem* AISTSimulatorItem::createBodyMotionController(BodyItem* bodyItem, BodyMotionItem* bodyMotionItem)
{
    return new HighGainControllerItem(bodyItem, bodyMotionItem);
}


bool AISTSimulatorItem::initializeSimulation(const std::vector<SimulationBody*>& simBodies)
{
    return impl->initializeSimulation(simBodies);
}


bool AISTSimulatorItemImpl::initializeSimulation(const std::vector<SimulationBody*>& simBodies)
{
    if(ENABLE_DEBUG_OUTPUT){
        static int ntest = 0;
        os.open((string("test-log-") + boost::lexical_cast<string>(ntest++) + ".log").c_str());
        os << setprecision(30);
    }

    if(integrationMode.is(AISTSimulatorItem::EULER_INTEGRATION)){
        world.setEulerMethod();
    } else if(integrationMode.is(AISTSimulatorItem::RUNGE_KUTTA_INTEGRATION)){
        world.setRungeKuttaMethod();
    }
    world.setGravityAcceleration(gravity);
    world.enableSensors(true);
    world.setTimeStep(self->worldTimeStep());
    world.setCurrentTime(0.0);

    ConstraintForceSolver& cfs = world.constraintForceSolver;

    cfs.setGaussSeidelErrorCriterion(errorCriterion.value());
    cfs.setGaussSeidelMaxNumIterations(maxNumIterations);
    cfs.setContactDepthCorrection(
        contactCorrectionDepth.value(), contactCorrectionVelocityRatio.value());

    world.clearBodies();
    bodyIndexMap.clear();
    for(size_t i=0; i < simBodies.size(); ++i){
        addBody(simBodies[i]);
    }

    cfs.setFriction(staticFriction, slipFriction);
    cfs.setContactCullingDistance(contactCullingDistance.value());
    cfs.setContactCullingDepth(contactCullingDepth.value());
    cfs.setCoefficientOfRestitution(epsilon);
    cfs.setCollisionDetector(self->collisionDetector());
    
    if(is2Dmode){
        cfs.set2Dmode(true);
    }

    world.initialize();

    return true;
}


void AISTSimulatorItemImpl::addBody(SimulationBody* simBody)
{
    DyBody* body = static_cast<DyBody*>(simBody->body());

    DyLink* rootLink = body->rootLink();
    rootLink->v().setZero();
    rootLink->dv().setZero();
    rootLink->w().setZero();
    rootLink->dw().setZero();
    rootLink->vo().setZero();
    rootLink->dvo().setZero();

    bool isHighGainMode = dynamicsMode.is(AISTSimulatorItem::HG_DYNAMICS);
    if(dynamic_cast<HighGainControllerItem*>(simBody->controller())){
        isHighGainMode = true;
    }

    for(int i=0; i < body->numLinks(); ++i){
        Link* link = body->link(i);
        link->u() = 0.0;
        link->dq() = 0.0;
        link->ddq() = 0.0;
    }
    
    body->clearExternalForces();
    body->calcForwardKinematics(true, true);

    if(isHighGainMode){
        ForwardDynamicsCBMPtr cbm = make_shared_aligned<ForwardDynamicsCBM>(body);
        cbm->setHighGainModeForAllJoints();
        bodyIndexMap[body] = world.addBody(body, cbm);
    } else {
        bodyIndexMap[body] = world.addBody(body);
    }
}


bool AISTSimulatorItem::stepSimulation(const std::vector<SimulationBody*>& activeSimBodies)
{
    impl->world.constraintForceSolver.clearExternalForces();

    if(!impl->dynamicsMode.is(KINEMATICS)){
        impl->world.calcNextState();
        return true;
    }

    // Kinematics mode
    if(!impl->isKinematicWalkingEnabled){
        for(size_t i=0; i < activeSimBodies.size(); ++i){
            activeSimBodies[i]->body()->calcForwardKinematics(true, true);
        }
    } else {
        for(size_t i=0; i < activeSimBodies.size(); ++i){
            Body* body = activeSimBodies[i]->body();
            LeggedBodyHelper* legged = getLeggedBodyHelper(body);
            if(!legged->isValid()){
                body->calcForwardKinematics(true, true);
            } else {
                Link* supportFoot = 0;
                const int n = legged->numFeet();
                for(int i=0; i < n; ++i){
                    Link* foot = legged->footLink(i);
                    if(!supportFoot || foot->p().z() < supportFoot->p().z()){
                        supportFoot = foot;
                    }
                }
                LinkTraverse traverse(supportFoot, true, true);
                traverse.calcForwardKinematics(true, true);
            }
        }
    }
    return true;
}


void AISTSimulatorItem::finalizeSimulation()
{
    if(ENABLE_DEBUG_OUTPUT){
        impl->os.close();
    }
}


void AISTSimulatorItem::doPutProperties(PutPropertyFunction& putProperty)
{
    SimulatorItem::doPutProperties(putProperty);
    impl->doPutProperties(putProperty);
}


void AISTSimulatorItemImpl::doPutProperties(PutPropertyFunction& putProperty)
{
    using boost::bind;
    
    putProperty(_("Dynamics mode"), dynamicsMode,
                bind((bool(Selection::*)(int))&Selection::select, &dynamicsMode, _1));
    putProperty(_("Integration mode"), integrationMode,
                bind((bool(Selection::*)(int))&Selection::select, &integrationMode, _1));
    putProperty(_("Gravity"), str(gravity), bind(toVector3, _1, boost::ref(gravity)));
    putProperty.decimals(3).min(0.0);
    putProperty(_("Static friction"), staticFriction, changeProperty(staticFriction));
    putProperty(_("Slip friction"), slipFriction, changeProperty(slipFriction));
    putProperty(_("Contact culling distance"), contactCullingDistance,
                (bind(&FloatingNumberString::setNonNegativeValue, boost::ref(contactCullingDistance), _1)));
    putProperty(_("Contact culling depth"), contactCullingDepth,
                (bind(&FloatingNumberString::setNonNegativeValue, boost::ref(contactCullingDepth), _1)));
    putProperty(_("Error criterion"), errorCriterion,
                bind(&FloatingNumberString::setPositiveValue, boost::ref(errorCriterion), _1));
    putProperty.min(1.0)(_("Max iterations"), maxNumIterations, changeProperty(maxNumIterations));
    putProperty(_("CC depth"), contactCorrectionDepth,
                bind(&FloatingNumberString::setNonNegativeValue, boost::ref(contactCorrectionDepth), _1));
    putProperty(_("CC v-ratio"), contactCorrectionVelocityRatio,
                bind(&FloatingNumberString::setNonNegativeValue, boost::ref(contactCorrectionVelocityRatio), _1));
    putProperty(_("Kinematic walking"), isKinematicWalkingEnabled,
                changeProperty(isKinematicWalkingEnabled));
    putProperty(_("2D mode"), is2Dmode, changeProperty(is2Dmode));
}


bool AISTSimulatorItem::store(Archive& archive)
{
    SimulatorItem::store(archive);
    return impl->store(archive);
}


bool AISTSimulatorItemImpl::store(Archive& archive)
{
    archive.write("dynamicsMode", dynamicsMode.selectedSymbol());
    archive.write("integrationMode", integrationMode.selectedSymbol());
    write(archive, "gravity", gravity);
    archive.write("staticFriction", staticFriction);
    archive.write("slipFriction", slipFriction);
    archive.write("cullingThresh", contactCullingDistance);
    archive.write("contactCullingDepth", contactCullingDepth);
    archive.write("errorCriterion", errorCriterion);
    archive.write("maxNumIterations", maxNumIterations);
    archive.write("contactCorrectionDepth", contactCorrectionDepth);
    archive.write("contactCorrectionVelocityRatio", contactCorrectionVelocityRatio);
    archive.write("kinematicWalking", isKinematicWalkingEnabled);
    archive.write("2Dmode", is2Dmode);
    return true;
}


bool AISTSimulatorItem::restore(const Archive& archive)
{
    SimulatorItem::restore(archive);
    return impl->restore(archive);
}


bool AISTSimulatorItemImpl::restore(const Archive& archive)
{
    string symbol;
    if(archive.read("dynamicsMode", symbol)){
        dynamicsMode.select(symbol);
    }
    if(archive.read("integrationMode", symbol)){
        integrationMode.select(symbol);
    }
    read(archive, "gravity", gravity);
    archive.read("staticFriction", staticFriction);
    archive.read("slipFriction", slipFriction);
    contactCullingDistance = archive.get("cullingThresh", contactCullingDistance.string());
    contactCullingDepth = archive.get("contactCullingDepth", contactCullingDepth.string());
    errorCriterion = archive.get("errorCriterion", errorCriterion.string());
    archive.read("maxNumIterations", maxNumIterations);
    contactCorrectionDepth = archive.get("contactCorrectionDepth", contactCorrectionDepth.string());
    contactCorrectionVelocityRatio = archive.get("contactCorrectionVelocityRatio", contactCorrectionVelocityRatio.string());
    archive.read("kinematicWalking", isKinematicWalkingEnabled);
    archive.read("2Dmode", is2Dmode);
    return true;
}
