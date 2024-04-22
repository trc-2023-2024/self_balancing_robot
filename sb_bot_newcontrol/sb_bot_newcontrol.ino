#include <ESP32Encoder.h>
#include <Wire.h>


// imu definitions
const uint8_t scl = 22; 
const uint8_t sda = 21; 
// imu angle data globals
float RateRoll, RatePitch, RateYaw;
float AccX, AccY, AccZ;
float AngleRoll, AnglePitch;

// kalman stuff
float KalmanAngleRoll=0, KalmanUncertaintyAngleRoll=2*2;
float KalmanAnglePitch=0, KalmanUncertaintyAnglePitch=2*2, ka_cal=0.0;
float Kalman1DOutput[]={0,0};

struct calImu{
  int RateCalibrationNumber;  
  float RateCalibrationRoll, RateCalibrationPitch, RateCalibrationYaw; // for gyro
  float RateCalibrationAR, RateCalibrationAP; // for accelerometer

}cal;


// kalman used for fusing accelerometry and gyro data (PS: params of kalman can be improved for better angle data feedback)
void kalman_1d(float KalmanState, float KalmanUncertainty, float KalmanInput, float KalmanMeasurement){
  KalmanState=KalmanState+0.004*KalmanInput;
  KalmanUncertainty=KalmanUncertainty + 0.004 * 0.004 * 4 * 4;
  float KalmanGain=KalmanUncertainty * 1/(1*KalmanUncertainty + 3 * 3);
  KalmanState=KalmanState+KalmanGain * (KalmanMeasurement-KalmanState);
  KalmanUncertainty=(1-KalmanGain) * KalmanUncertainty;
  Kalman1DOutput[0]=KalmanState; 
  Kalman1DOutput[1]=KalmanUncertainty;
}

void imu_signals(void){
  //0x68 --> SIGNAL_PATH_RESET
  Wire.beginTransmission(0x68);  // waking up the imu and reseting gyro/accel/temp sensors
  // 0x1A --> CONFIG to config external DLPF for gyro and accele
  Wire.write(0x1A); // switching on the low pass filter 
  Wire.write(0x05);
  Wire.endTransmission();

  // accelerometer part
  Wire.beginTransmission(0x68);
  // 0x1C --> ACCEL_CONFIG (same full scale configuration setting like in gyroscope mentioned below)
  Wire.write(0x1C);
  Wire.write(0x10); // 0x10 full scale setting of +-8g
  Wire.endTransmission();

  // accessing accelerometer data
  Wire.beginTransmission(0x68);
  // 0x3B --> ACCEL_XOUT register
  Wire.write(0x3B);
  Wire.endTransmission(); 
  Wire.requestFrom(0x68,6); // requesting 6 raw accele data like gyro
  // raw data processing defined after gyro raw data processing(given down)
  int16_t AccXLSB = Wire.read() << 8 | Wire.read();
  int16_t AccYLSB = Wire.read() << 8 | Wire.read();
  int16_t AccZLSB = Wire.read() << 8 | Wire.read();


  // gyroscope part (PS: temperature sensor data was ignored hence the elaborate wire.endTransmission usage after every data accessing)
  Wire.beginTransmission(0x68);
  // 0x1B --> GYRO_CONFIG register (here 0x8 is for fullscale reading of +-500 deg/s)
  Wire.write(0x1B);
  Wire.write(0x8); // setting scale sens factor for gyro(use corresponding full scale factor for conversion)
  Wire.endTransmission();

  // accessing gyro raw data for mpu6050
  Wire.beginTransmission(0x68);
  // 0x43 --> GYRO_XOUT register (where to starting taking gyro values from)
  Wire.write(0x43);
  Wire.endTransmission();
  Wire.requestFrom(0x68, 6); // request 6 gyro data(gx(high,low),gy(,,),gz(,,))
  int16_t GyroX=Wire.read()<<8 | Wire.read();
  int16_t GyroY=Wire.read()<<8 | Wire.read();
  int16_t GyroZ=Wire.read()<<8 | Wire.read();
  RateRoll=(float)GyroX/65.5; //+-500 deg/s (65.5 LSB/deg/s) --> conversion factor to deg/s
  RatePitch=(float)GyroY/65.5;
  RateYaw=(float)GyroZ/65.5;

  AccX=(float)AccXLSB/4096;
  AccY=(float)AccYLSB/4096;
  AccZ=(float)AccZLSB/4096;

  // PS: gyro data in deg/s units
  // Raw Accel data processing
  AngleRoll=atan(AccY/sqrt(AccX*AccX+AccZ*AccZ))*1/(3.142/180);
  AnglePitch=-atan(AccX/sqrt(AccY*AccY+AccZ*AccZ))*1/(3.142/180);
  // AnglePitch and AngleRoll is in degree units

  // PS: explict request of 6 raw data is required to prevent the imu from giving unwanted data

}


// Motor Pins and Encoder Defintions (cytron MDD10A)
// motor-left
const int motorPinDirectionLeft = 18; 
const int motorPinSpeedLeft = 19;  
const int encoderPinA_Left = 4;  
const int encoderPinB_Left = 16;

//motor-right
const int motorPinDirectionRight = 13;  
const int motorPinSpeedRight = 14;  
const int encoderPinA_Right = 2;  
const int encoderPinB_Right = 15;

ESP32Encoder enLeft;
ESP32Encoder enRight;


//**************************************************** Global Definitions ********************************************
struct PIDGains{
  float kp, ki, kd;
};

struct TimeDefs{
  float deltaT;
  long currT;
  long prevT=0;
};

struct errParamDef{
  float errPrev_=0.0;
  float err_;
  float eintegral_=0.0;
  float set_point;
};


struct enParams{
  volatile int pos=0;
  volatile int posPrev = 0;
  volatile float velocity=0.0;
  float vel_rpm = 0.0;
  float vel_filt = 0.0;
  float vel_prev_filt = 0.0;
  float vel_targ = 10.0;
  float uControl = 0.0;
};


uint32_t LoopTimer; // 1/controlLoop_frequency
enParams Left, Right;
TimeDefs tLeft, tRight;
PIDGains LeftPID,RightPID, BalancePID;
errParamDef LeftErr, RightErr;


void setup() {
  Serial.begin(57600); // baud rates affects the rate of data recieved by the arduino/esp32/esp82 not the control algo

  //***********************************************************************************************
  //Setting the PID parameters:

  LeftPID.kp = 4.0;
  LeftPID.ki = 1.0;
  LeftPID.kd = 0.0;

  RightPID.kp = 5.0;
  RightPID.ki = 1.0;
  RightPID.kd = 0.0;

  BalancePID.kp = 150.0;
  BalancePID.ki = 0.0;
  BalancePID.kd = 5.0;

  //***********************************************************************************************


  // setting up pins and pin modes of motor
  pinMode(motorPinDirectionLeft, OUTPUT);
  pinMode(motorPinSpeedLeft, OUTPUT);
  pinMode(motorPinDirectionRight, OUTPUT);
  pinMode(motorPinSpeedRight, OUTPUT);
  Serial.println("Motor Pins Defined!");

 pinMode(25,OUTPUT); //For checking purpose GPIO14 - D25(recheck for esp32)

  //************************************ Encoder setup *******************************************
  Serial.println("Encoder Setting up...");
  ESP32Encoder::useInternalWeakPullResistors = puType::up;

  enLeft.attachFullQuad(encoderPinA_Left, encoderPinB_Left);  // Left motor
  enRight.attachFullQuad(encoderPinA_Right, encoderPinB_Right); // Right Motor
  // set the encoder counts to 0(begin) --> may have some offsets (make sure the mag-hall effect encoder is properly installed)
  enLeft.setCount(0);
  enRight.setCount(0);
  // for verification (may or maynot uncomment)
  // Serial.println("Encoder Left Start = " + String((int32_t)enLeft.getCount()));
  // Serial.println("Encoder Right Start = " + String((int32_t)enRight.getCount()));

  //********************************************** IMU Setup ****************************************

  Wire.setClock(400000); // 400kHz (fast mode I2C communications) 
  // PS: this mode for I2C is more than enough for mpu6050 which has a 1kHz data transfer freq.(approx. I think)
  Wire.begin(sda, scl);
  delay(250);
  Wire.beginTransmission(0x68); // reset imu
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();
  Serial.println("Calibrating gyro......");
  // calibration of offsets
  for (cal.RateCalibrationNumber=0; cal.RateCalibrationNumber< 2000; cal.RateCalibrationNumber++) {
    imu_signals();
    Serial.print(".");
    cal.RateCalibrationRoll += RateRoll;
    cal.RateCalibrationPitch += RatePitch;
    cal.RateCalibrationYaw += RateYaw;
    delay(1);
    
  }
  Serial.println("Gyro Calibration over...");
  cal.RateCalibrationRoll/=2000;
  cal.RateCalibrationPitch/=2000;
  cal.RateCalibrationYaw/=2000;

  Serial.println("Calibrating accelerometer...");
  for (cal.RateCalibrationNumber=0; cal.RateCalibrationNumber< 2000; cal.RateCalibrationNumber++) {
    imu_signals();
    Serial.print(".");
    cal.RateCalibrationAR += AngleRoll;
    cal.RateCalibrationAP += AnglePitch;
    delay(1);
  }
  Serial.println("Accelerometer Calibration over...");
  cal.RateCalibrationAR/=2000;
  cal.RateCalibrationAP/=2000;
  
  LoopTimer=micros();
  tLeft.prevT = LoopTimer;
  tRight.prevT = LoopTimer;

}



void loop() {

  //****************************************************************************** getting encoder data ********************************************************
  tLeft.currT = micros();
  Left.pos = enLeft.getCount(); 
  tLeft.deltaT = ((float) (tLeft.currT - tLeft.prevT)) / 1.0e6;
  Left.velocity = (Left.pos - Left.posPrev) / tLeft.deltaT; // in cps units
  Left.posPrev = Left.pos;
  tLeft.prevT = tLeft.currT;

  tRight.currT = micros();
  Right.pos = enRight.getCount();
  tRight.deltaT = ((float) (tRight.currT - tRight.prevT)) / 1.0e6;
  Right.velocity = (Right.pos - Right.posPrev) / tRight.deltaT;
  Right.posPrev = Right.pos;
  tRight.prevT = tRight.currT;
  trig_pin();
  // ************************************************************************************************************************************************************
  // delay(50); // don't know if this delay is technical needed but for safety keeping it for first testing

  // ******************************************************************************* getting imu data ********************************************************* 
 
  imu_signals();
  RateRoll-=cal.RateCalibrationRoll;
  RatePitch-=cal.RateCalibrationPitch;
  RateYaw-=cal.RateCalibrationYaw;
  AngleRoll-=cal.RateCalibrationAR;
  AnglePitch-=cal.RateCalibrationAP;

  kalman_1d(KalmanAngleRoll, KalmanUncertaintyAngleRoll, RateRoll, AngleRoll);
  KalmanAngleRoll=Kalman1DOutput[0]; 
  KalmanUncertaintyAngleRoll=Kalman1DOutput[1];
  kalman_1d(KalmanAnglePitch, KalmanUncertaintyAnglePitch, RatePitch, AnglePitch);
  KalmanAnglePitch=Kalman1DOutput[0]; 
  KalmanUncertaintyAnglePitch=Kalman1DOutput[1];
  // *************************************************************************************************************************************************************

  //*********************************************Applying PID on the wheel velocity********************************************************************************************

 // Converting the velocity to RPM, the conversion factor may vary based on the CPR and gear ration
  Left.vel_rpm = (Left.velocity * 60)/(34560); // GearRatio=540:1, CPR=32
  Right.vel_rpm = (Right.velocity * 60)/(17280); // GearRatio=540:1, CPR=16  

  //Applying filter to the velocity

  Left.vel_filt = lowPassFilter(Left.vel_filt,Left.vel_rpm, Left.vel_prev_filt);
  Right.vel_filt = lowPassFilter(Right.vel_filt,Right.vel_rpm, Right.vel_prev_filt); // 25HZ cuttoff  

  Left.uControl = pidControl(Left.vel_targ, Left.vel_filt, LeftPID.kp, LeftPID.ki, LeftPID.kd, LeftErr.err_, LeftErr.errPrev_, LeftErr.eintegral_, tLeft.deltaT);
  Right.uControl = pidControl(Right.vel_targ, Right.vel_filt, RightPID.kp, RightPID.ki, RightPID.kd, RightErr.err_, RightErr.errPrev_, RightErr.eintegral_, tRight.deltaT);

   if(Right.uControl >= 30.0){
    Right.uControl = 30.0;
  }
  if(Left.uControl >= 30.0){
    Left.uControl = 30.0;
  }

   if(Right.uControl <= -30.0){
    Right.uControl = -30.0;
  }
   if(Left.uControl <= -30.0){
    Left.uControl = -30.0;
  }
  // motor motion and PID-control algo(here)
  setMotor(Left.uControl, motorPinSpeedLeft, motorPinDirectionLeft);
  setMotor(Right.uControl, motorPinSpeedRight, motorPinDirectionRight);
  //

  // encoder data
  Serial.print("deltaLeft:");
  Serial.print(tLeft.deltaT);
  Serial.print(",");
  Serial.print("deltaRight:");
  Serial.print(tRight.deltaT);
  Serial.print(",");
  // Serial.print("LCPS:");
  // Serial.print(Left.velocity);
  // Serial.print("RCPS:");
  // Serial.print(Right.velocity);
  // Serial.print(",");
  // Serial.print("Left RPM:");
  // Serial.print(Left.vel_rpm);
  // Serial.print(",");
  // Serial.print("Right RPM:");
  // Serial.print(Right.vel_rpm);
  // Serial.print(",");
  Serial.print("Left RPMF:");
  Serial.print(Left.vel_filt);
  Serial.print(",");
  Serial.print("Right RPMF:");
  Serial.print(Right.vel_filt);
  Serial.print(",");
  Serial.print("LeftControlVel:");
  Serial.print(Left.uControl);
  Serial.print(",");
  Serial.print("RightControlVel:");
  Serial.print(Right.uControl);
  Serial.print(",");
  Serial.print("Target RPM:");
  Serial.print(Right.vel_targ);
  Serial.print(",");
  // // Serial.println();

  // // imu data

  Serial.print("KalmanPitch:");
  Serial.print(KalmanAnglePitch);
  Serial.print(",");
  Serial.print("errL:");
  Serial.print(LeftErr.err_*60);
  Serial.print(",");
  Serial.print("errR:");
  Serial.print(RightErr.err_*60);
  Serial.println();

  while (micros() - LoopTimer < 4000);
  LoopTimer=micros();

  delay(10);
}

// custom function defintions here 
void setMotor(float uControl, int in1, int in2) {
  int dir = (uControl >= 0) ? 1 : -1;
  // MAPPING pwm->rpm
  // int pwmVal = min(255, abs((int)uControl));
  int pwmVal = map(abs(int(uControl)), 0, 30, 0, 255);
  analogWrite(in1, pwmVal); // Motor speed
  digitalWrite(in2, (dir == 1) ? HIGH : LOW);
  // Serial.print("pwm:");
  // Serial.print(pwmVal);
  // Serial.println();
}


float pidControl(float vTar_, float vAct_, float kp_, float ki_, float kd_, float &err_, float &errPrev_, float &eintegral_, float dt_) {
  err_ = (vTar_ - vAct_)/60;
  eintegral_ += (dt_) * (err_);
  float uControl = kp_ * err_ + ki_ * eintegral_ + ((kd_ * (err_ - errPrev_)/(dt_)));
  errPrev_ = err_;
  return uControl*60;
}

// low-pass filter function(25Hz- cuttoff freq --> needs experimental verification to use a better cuttoff frequency)
float lowPassFilter(float &v1Filt_, float v1_, float &v1Prev_) {
  v1Filt_ = 0.854 * v1Filt_ + 0.0728 * v1_ + 0.0728 * v1Prev_;
  // float v1Filt_ = (v1Filt_ + v1_ + v1Prev_)/3; // cuttoff freq not decideds for taking running avg..
  v1Prev_ = v1_;
  return v1Filt_;
}

void trig_pin(){
  digitalWrite(25,HIGH);
  delay(1);
  digitalWrite(25,LOW);
}