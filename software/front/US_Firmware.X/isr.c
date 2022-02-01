#define _ISR_C
#include <xc.h>
#include <GenericTypeDefs.h>
#include <peripheral/i2c.h>
#include <peripheral/adc10.h>
#include "mcu.h"
#include "isr.h"
#include "pars.h"
#include "main.h"
#include "iron.h"
#include "PID.h"
#include "io.h"

volatile unsigned int ISRComplete = 0;

volatile int I2CCommands;
volatile int I2CCCommand;

void ISRInit(){
    int i;
    ISRStep = 0;
    ADCStep = 0;
    ISRTicks = 1;
    CJTicks = 0;
    PHEATER = 0;
    I2CStep = 0;
    I2CCommands = 0;
    I2CCCommand = 0;
    I2CIdle = 1;
    ISRStopped = 0;
    EEPAddrR = 0xFFFF;
    EEPDataR = 0;
    EEPCntR = 0;
    EEPAddrW = 0xFFFF;
    EEPDataW = 0;
    EEPCntW = 0;
    for(i = 2; i--;){
        PIDVars[i].NoHeater = 255;
        PIDVars[i].NoSensor = 255;
        PIDVars[i].ShortCircuit = 255;
        PIDVars[i].HInitData = 1;
        PIDVars[i].OffDelay = 1600;
    }
    OffDelayOff = 1600;
    VTIBuffCnt = 0;
    mainFlags.Calibration = 0;
    mainFlags.PowerLost = 0;    
    ISRComplete = 0;
}

void ISRStop(){
    ISRStopped = 1;
    while(!(ISRStopped & 2));
    while(!I2CIdle);
    mcuADCStop();
    mcuStopISRTimer();
    mcuCompDisable();
    mainFlags.PowerLost = 0;
}

void ISRStart(){
    mcuDisableInterrupts();
    mcuCompDisable();
    mcuADCStop();
    mcuStopISRTimer();
    if(mainFlags.ACPower){
        while(MAINS);
        while(!MAINS);
        _delay_us(1000);
    }
    mcuDCTimerReset();
    mcuCompEnableH2L();
    VTIBuffCnt = 0;
    ISRStopped = 0;
    mainFlags.PowerLost = 0;
    mcuEnableInterrupts();
}

void I2CAddCommands(int c){
    int i;
    i=mcuDisableInterrupts();
    I2CCommands |= c;
    if(I2CIdle)mcuI2CWakeUp();
    mcuRestoreInterrupts(i);
}

void OnPowerLost(){
    if(!mainFlags.PowerLost){
        mainFlags.PowerLost = 1;
        OLED_VCC = 1; //turn off OLED's power
        HEATER = 0;
        mcuADCStop();
        mcuStopISRTimer();
        mcuCompDisable();
        ISRStep = 0;
        ADCStep = 0;
    }    
}

void ISRHigh(int src){
    static int OldHeater;
    t_PIDVars *PV;
    t_IronConfig *IC;
    UINT32 dw;

    switch(src){
        case CompH2L:
            if(!mainFlags.ACPower) OnPowerLost();
        case DCTimer:
            if(ISRStep != 11)ISRComplete = 0;
            ISRStep = 0;
            break;
        case CompL2H:
            return;
    }

    PV = (t_PIDVars *)&PIDVars[1];
    IC = (t_IronConfig *)&IronPars.Config[1];
    if((ADCStep < 2) || (IC->SensorConfig.Type == SENSOR_UNDEFINED)){
        PV = (t_PIDVars *)&PIDVars[0];
        IC = (t_IronConfig *)&IronPars.Config[0];
    }

    switch (ISRStep){
        case 0: //AC - high to low point of comparator (around 4.5V), DC - DC Timer interrupt (110Hz);
            OldHeater = HEATER;
            if(ISRStopped){
                HEATER = 0;
                if(!(ISRStopped & 2)){
                    ISRStopped |= 2;
                    mcuADCStop();
                    VTIBuffCnt = 0;
                }
            }
            else{
                mcuCompDisable();
                if(mainFlags.PowerLost) return;
                INT32 d = OffDelayOff;
                if(HEATER && d < PV->OffDelay) d = PV->OffDelay;
                d >>= 4;
                d-=300; //260us before zero cross + 40us to compensate group delay of R46-R43-C60 6367Hz R-C filter (25us) + interrupt latency (around 15us)
                if(d < 10) d = 10;
                if(d > 1000) d = 1000;
                mcuStartISRTimer_us(d);
            }
            break;
        case 1: //260us before AC zero cross - turn off power, prepare ADC, Voltage reference, calculate heater resistance, input voltage, current and power.
            PGD = 1;
            mcuStartISRTimer_us(50);
            HEATER = 0;
            mcuADCStartManualVRef();
            break;
        case 2: //210us before AC zero cross - setup channels, calculate voltage, current, power and heater resistance
            if(mainFlags.ACPower){
                mcuStartISRTimer_us(250);
            }
            else{
                mcuStartISRTimer_us(550);
            }
            
            if(mainFlags.Calibration){
                CHSEL1 = CalCh ? 0 : 1;
                CHSEL2 = CalCh ? 1 : 0;
                CHPOL = CalCh ? 0 : 1;
            }
            else{
                CHSEL1 = IC->SensorConfig.InputP;
                CHSEL2 = IC->SensorConfig.InputN;
                CHPOL = IC->SensorConfig.InputInv;
            }                        
            
            if(ISRComplete){
                if((dw = VTIBuffCnt >> 2) && (VBuff[dw] < 90) && OldHeater && (TIBuff[dw] < 16)){ //power lost if <0.5A and <7.4V
                    OnPowerLost();
                    ISRStep = 0;
                    return;
                }
                if(ADCStep & 1){
                    if(mainFlags.ACPower && CompLowTime){          //calculate delay from comparator trigger to zero current (just before zero cross)
                        dw = OldHeater ? PV->OffDelay : OffDelayOff;
                        if(dw <= 1600) dw = CompLowTime << 4;                                               
                        dw -= dw >> 4;
                        dw += CompLowTime;

                        if(OldHeater && PV->Power && CompLowTimeOn && CompLowTimeOff && CompLowTimeOn > CompLowTimeOff){
                            dw += CompLowTimeOn - CompLowTimeOff; //when heater was on 1/2 or 1/4 power, comp low time is shorter then on full power - compensate for it;
                        }

                        if(dw < 1600) dw = 1600;

                        if(OldHeater){
                            PV->OffDelay = dw;                                
                        }
                        else{
                            OffDelayOff = dw;
                        }
                    }
                    else{
                        OffDelayOff = PV->OffDelay = 1600;
                    }
                    CompLowTime = 0;
                }
                else{
                    if(OldHeater){
                        UINT32 i, l;
                        if((i = l = VTIBuffCnt) > 1){
                            UINT32 sv = 0, si = 0, sp = 0, ri = 0, rv = 0, r = 0;
                            for(i--, l--; i--;){
                                UINT32 ci = TIBuff[i];
                                UINT32 cv = VBuff[i] + VBuff[i + 1];
                                if(ci < 1023){
                                    if(ri < ci && VBuff[i] < 1023 && VBuff[i + 1] < 1023){
                                        ri = ci;
                                        rv = cv;
                                        r = 0;
                                    }
                                }
                                else{
                                    if(!r && ri) r = ((rv * 6738) / ri) >> 9;
                                    if(r) ci = ((cv * 6738) / r) >> 9;
                                }
                                sv += cv * cv;
                                si += ci * ci;
                                sp += ci * cv;
                            }                            

                            r = (ri ? (((rv * 6738) / ri) + 256) >> 9 : 0x7FFF);
                            // 6738 = 10 * 256 * (((Rs1 * (R48 / R42)) / VRef) * 1024) / (((R51 / (R47 + R51)) / VRef) * 1024)
                            //      = 10 * 256 * (((0.003 * (47K / 1.5K)) / 3.0V) *1024) / (((1K / (27K + 1K)) / 3.0V) * 1024)
                            //      = 6737.92

                            sv = (sv + (l >> 1)) / l;
                            si = (si + (l >> 1)) / l;
                            l *= (UINT32)(391 * 2);
                            sp = (sp + (l >> 1)) / l;
                            // 391 = (((R51 / (R47 + R51)) / VRef) * 1024) * (((Rs1 * (R48 / R42)) / VRef) * 1024)
                            //     = (((1K / (27K + 1K)) / 3.0V) * 1024) * (((0.003 * (47K / 1.5K)) /3.0V) * 1024)
                            //     = 391,13549206349206349206349206349

                            dw = POWER_DUTY;
                            if(IronPars.Config[1].SensorConfig.Type) dw >>= 1;
                            PV->HV = (mcuSqrt(sv * dw) + 31) >> 6;
                            PV->HI = (mcuSqrt(si * dw) + 15) >> 5;
                            PV->HP = ((sp * dw) + 511) >> 10;
                            PV->HR = r;                             

                            PV->HNewData = 1;
                        }
                    }
                    else if(mainFlags.TipChange){
                        PV->HI = 0;
                        PV->HR = 5000;
                        PV->HP = 0;
                        PV->HInitData = 1;
                        PV->HNewData = 1;
                    }
                }
            }
            VTIBuffCnt = 0;
            break;
        case 3: //40us after zero cross - read room temperature or iron holder voltage level
            mcuADCRead((ADCStep & 1) ? ADCH_RT : ADCH_HOLDER,1);
            break;
        case 4: //65us after zero cross - start reading iron temperature.
            mcuADCRead(ADCH_TEMP, 4);
            if(ADCStep <= 1){
                if(ADCStep & 1){
                    ADCData.VRT = mcuADCRES;
                }
                else{
                    ADCData.VH = mcuADCRES;
                }
            }
            else{
                if(ADCStep & 1){
                    ADCData.VRT += mcuADCRES;
                }
                else{
                    ADCData.VH += mcuADCRES;
                }
            }
            break;
        case 5: //125us after zero cross - check for sensor open, heater open, perform PID and wait to 1/2 power point (AC voltage point just between 2 adjacent zero crosses)
            mcuStartISRTimer_us(125);
            if(ISRComplete && !(ADCStep & 1)) mcuCompEnableL2H();
            ISRComplete = 0;
            if(!mainFlags.Calibration){
                CHSEL1 = 0;
                CHSEL2 = 0;
            }            
            ADCData.HeaterOn = PHEATER;
            ADCData.VTEMP[ADCStep & 1] = mcuADCRES >> 2;
            
            if(ADCStep & 1) {
                PGC=1;
                if(PV->HR > 3000) {
                    if(PV->NoHeater < 255) PV->NoHeater++;
                }
                else{
                    PV->NoHeater = 0;
                }
                if(PV->HR < 8){
                    if(PV->ShortCircuit < 255) PV->ShortCircuit++;
                }
                else{
                    PV->ShortCircuit = 0;
                }
                if(ADCData.VTEMP[1] >= 1023){
                    if(PV->NoSensor < 255) PV->NoSensor++;
                }
                else{
                    PV->NoSensor = 0;
                }
                PID(ADCStep >> 1);
                if(PV->KeepOff)PV->KeepOff--;
                PGC=0;
            }

            ADCStep = (ADCStep + 1) & 3;
            
            PV = (t_PIDVars *)&PIDVars[1];
            IC = (t_IronConfig *)&IronPars.Config[1];
            if(ADCStep < 2 || IC->SensorConfig.Type == SENSOR_UNDEFINED){
                PV = (t_PIDVars *)&PIDVars[0];
                IC = (t_IronConfig *)&IronPars.Config[0];
            }            
            HCH = IC->SensorConfig.HChannel;
            break;
        case 6: //250us (or 550us on  DC) after zero cross - turn on power if needed, setup channels for handle sensor if present and wait to 1/2 power point at the middle of half period
            dw = MAINS_PER_H_US - 250;
            mcuStartISRTimer_us(dw); //next step will be at the center of mains half period
            if(!(ADCStep & 1)){
                PV->PWM += PV->PIDDuty;
                PHEATER = ((PV->PWM>>24)!=0);
                PV->PWM &= 0x00FFFFFF;
            }            
            if(mainFlags.PowerLost || PV->KeepOff || mainFlags.Calibration || IC->SensorConfig.Type == SENSOR_UNDEFINED || IC->SensorConfig.Type == SENSOR_NONE ) PHEATER = 0;
            if(!PV->Power) HEATER = PHEATER;  //Turn on heater if on full power    
            PGD = 0;

            mcuADCStartAutoVRef(PHEATER?0:1);                        
            if(!mainFlags.Calibration && !PHEATER && !CJTicks && IronPars.ColdJunctionSensorConfig && IronPars.ColdJunctionSensorConfig->HChannel == IC->SensorConfig.HChannel){
                CHSEL1 = IronPars.ColdJunctionSensorConfig->InputP;
                CHSEL2 = IronPars.ColdJunctionSensorConfig->InputN;
                CHPOL = IronPars.ColdJunctionSensorConfig->InputInv;
                CBANDA = IronPars.ColdJunctionSensorConfig->CBandA;
                CBANDB = IronPars.ColdJunctionSensorConfig->CBandB;
                I2CData.CurrentA.ui16 = IronPars.ColdJunctionSensorConfig->CurrentA;
                I2CData.CurrentB.ui16 = IronPars.ColdJunctionSensorConfig->CurrentB;
                I2CData.Gain.ui16 = IronPars.ColdJunctionSensorConfig->Gain;
                I2CData.Offset.ui16 = IronPars.ColdJunctionSensorConfig->Offset;
                if(!mainFlags.PowerLost)I2CAddCommands(I2C_SET_CPOT | I2C_SET_GAINPOT | I2C_SET_OFFSET);
            }
            
            if(ADCStep == 0){
                if(BoardVersion == BOARD_HW_5_2){
                    Holder = NAP ? 1023 : 0;
                }
                else{
                    Holder = ADCData.VH >> 1;
                }
            }
            break;
        case 7:  //1/2 power point, AC voltage highest point (center of half period) - check for power lost and turn on heater if 1/2 power and wait for 1/4 power point
            mcuStartISRTimer_us(MAINS_PER_Q_US); //Next step will be at 1/4 power point in the mains period
            if(!MAINS || ((VBuff[dw = (mcuTIBuffPos() >> 2) - 1] < 90) && PHEATER && (TIBuff[dw] < 16))){ //power lost if <0.5A and <7.4V
                OnPowerLost();
                return;
            }
            if(PV->Power < 2) HEATER = PHEATER;                
            if(!mainFlags.Calibration){
                CHSEL1 = 0;
                CHSEL2 = 0;  
            }
            CHPOL = IC->SensorConfig.InputInv;
            CBANDA = IC->SensorConfig.CBandA;
            CBANDB = IC->SensorConfig.CBandB;
            I2CData.CurrentA.ui16 = IC->SensorConfig.CurrentA;
            I2CData.CurrentB.ui16 = IC->SensorConfig.CurrentB;
            I2CData.Gain.ui16 = IC->SensorConfig.Gain;
            I2CData.Offset.ui16 = IC->SensorConfig.Offset;
            if(!mainFlags.PowerLost)I2CAddCommands(I2C_SET_CPOT | I2C_SET_GAINPOT | I2C_SET_OFFSET);
            
            if(!mainFlags.Calibration && !PHEATER && !CJTicks && IronPars.ColdJunctionSensorConfig && IronPars.ColdJunctionSensorConfig->HChannel == IC->SensorConfig.HChannel){
                ADCData.VCJ = TIBuff[(mcuTIBuffPos() >> 2) - 1];
                CJTicks = CJ_PERIOD;
            }
            else{
                ADCData.VCJ = 0;
                if(!IronPars.ColdJunctionSensorConfig) CJTicks = 0;
            }
            break;
        case 8: //1/4 power point - turn on heater if needed
            mcuStartISRTimer_us(MAINS_PER_E_US); //Next step will be at 1/8 power point in the mains period
            if(PV->Power < 3) HEATER = PHEATER;                
            break;
        case 9: //1/8 power point - turn on heater if needed
            mcuStartISRTimer_us(200);
            HEATER = PHEATER;
            break;
        case 10: //at least 200uS after power was turned on - enable high-to-low comparator event in order to start new cycle.
            mcuCompEnableH2L();
            ISRTicks++;
            if(CJTicks) CJTicks--;
            ISRComplete = 1;
            break;
    }
    if(ISRStep<255)ISRStep++;    
}

void I2CISRTasks(){
    
    int i;
    UINT16 ui;
    BOOL CmdOK;

    if(I2CCCommand){
        I2CStep++;
    }
    else{
        I2CStep=0;
        i=mcuDisableInterrupts();
        if(I2CCommands &= 0xFF){
            I2CCCommand = 1;
            while(I2CCCommand &= 0xFF){
                if(I2CCommands & I2CCCommand){
                    I2CCommands -= I2CCCommand;
                    I2CIdle=0;
                    break;
                }
                I2CCCommand <<= 1;
            }
        }
        else{
            I2CIdle=1;
        }
        mcuRestoreInterrupts(i);
    }
    
    CmdOK = TRUE;
    switch(I2CCCommand){
        case I2C_SET_CPOT:
            switch(I2CStep){
                case 0:
                    mcuI2CStart();
                    break;
                case 1:
                    mcuI2CSendAddrW(CPOT);
                    break;
                case 2:
                    mcuI2CSendByte(0x40); //write to TCON, disable general ca??
                    break;
                case 3:
                    ui=0xFF;
                    if(I2CData.CurrentA.ui16 == 0) ui &= 0xF0;      //disconnect R0A when currentA is 0
                    if(I2CData.CurrentA.ui16 >= 256) ui &= 0xFE;    //disconnect R0B when currentA is MAX
                    if(I2CData.CurrentB.ui16 == 0) ui &= 0x0F;      //disconnect R1A when currentB is 0
                    if(I2CData.CurrentB.ui16 >= 256) ui &=0xEF;     //disconnect R1B when currentB is MAX
                    mcuI2CSendByte(ui); //write to TCON
                    break;
                case 4:
                    mcuI2CSendByte(I2CData.CurrentA.ui16 >> 8); //Wiper0
                    break;
                case 5:
                    mcuI2CSendByte(I2CData.CurrentA.ui16 & 0xFF);
                    break;
                case 6:
                    mcuI2CSendByte((I2CData.CurrentB.ui16 >> 8) | 0x10); //Wiper1
                    break;
                case 7:
                    mcuI2CSendByte(I2CData.CurrentB.ui16 & 0xFF);
                    break;
                case 8:
                    I2CCCommand=0;
                    mcuI2CStop();
                    break;
            }
            break;
        case I2C_SET_GAINPOT:
            switch(I2CStep){
                case 0:
                    mcuI2CStart();
                    break;
                case 1:
                    mcuI2CSendAddrW(GAINPOT);
                    break;
                case 2:
                    mcuI2CSendByte(I2CData.Gain.ui16 >> 8);
                    break;
                case 3:
                    mcuI2CSendByte(I2CData.Gain.ui16 & 0xFF);
                    break;
                case 4:
                    I2CCCommand=0;
                    mcuI2CStop();
                    break;
            }
            break;
        case I2C_SET_OFFSET:
            switch(I2CStep){
                case 0:
                    mcuI2CStart();
                    break;
                case 1:
                    mcuI2CSendAddrW(OFFADC);
                    break;
                case 2:
                    mcuI2CSendByte(0b01011000);
                    break;
                case 3:
                    mcuI2CSendByte(I2CData.Offset.ui16 >> 2);
                    break;
                case 4:
                    mcuI2CSendByte((I2CData.Offset.ui16 << 6) & 0xFF);
                    break;
                case 5:
                    I2CCCommand=0;
                    mcuI2CStop();
                    break;
            }
            break;
        case I2C_EEPWRITE:
            switch(I2CStep){
                case 0:
                    mcuI2CStart();
                    break;
                case 1:
                    mcuI2CSendAddrW(EEP);
                    break;
                case 2:
                    if((CmdOK = mcuI2CIsACK()))mcuI2CSendByte(EEPAddrW >> 8);
                    break;
                case 3:
                    mcuI2CSendByte(EEPAddrW & 0xFF);
                    break;
                case 4:
                    mcuI2CSendByte(*EEPDataW);
                    break;
                case 5:
                    EEPCntW--;
                    if(EEPCntW){
                        EEPDataW++;
                        EEPAddrW++;
                        if(((EEPAddrW & 31) == 0) || (I2CCommands & (I2CCCommand-1))){ //Abort if EEPROM page boundary is crossed, or if there is pending higher priority command
                            CmdOK = FALSE;
                        }
                        else {
                            I2CStep = 4;
                            mcuI2CSendByte(*EEPDataW);
                        }
                    }
                    else{
                        EEPAddrW=0xFFFF;
                        I2CCCommand=0;
                        mcuI2CStop();
                    }
                    break;
            }
            break;
        case I2C_EEPREAD:
            switch(I2CStep){
                case 0:
                    mcuI2CStart();
                    break;
                case 1:
                    mcuI2CSendAddrW(EEP);
                    break;
                case 2:
                    if((CmdOK = mcuI2CIsACK()))mcuI2CSendByte(EEPAddrR >> 8);
                    break;
                case 3:
                    mcuI2CSendByte(EEPAddrR & 0xFF);
                    break;
                case 4:
                    mcuI2CStart();
                    break;
                case 5:
                    mcuI2CSendAddrR(EEP);
                    break;
                case 6:
                    mcuI2CReceiverEnable();
                    break;
                case 7:
                    *EEPDataR = mcuI2CGetByte();
                    EEPCntR--;
                    if(EEPCntR){
                        EEPDataR++;
                        EEPAddrR++;
                        if(((EEPAddrR & 31) == 0) || (I2CCommands & (I2CCCommand - 1))){ //Abort if EEPROM page boundary is crossed, or if there is pending higher priority command
                            mcuI2CReceiverDisable();
                            CmdOK = FALSE;                            
                        }
                        else{
                            mcuI2CACK();
                            I2CStep = 5;
                        }
                    }
                    else{
                        mcuI2CReceiverDisable();
                        EEPAddrR = 0xFFFF;
                        I2CCCommand=0;
                        mcuI2CStop();
                    }
                    break;
            }
            break;
        default:
            I2CCCommand=0;
            break;
    }
    if(CmdOK == FALSE){
        I2CCommands |= I2CCCommand;
        I2CCCommand=0;
        mcuI2CStop();
    }    
}

#undef _ISR_C
