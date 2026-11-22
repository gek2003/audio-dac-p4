# ES9039Q2M — раздел Reference Schematics (даташит v0.2.3, стр. 95–99)

Подпись под этими схемами в даташите: «Schematic is representative of ES9039Q2M EVB v2.1».

## PDF-страница 94

```
VERSION 0.2.3 
 
ES9039Q2M Product Datasheet 
ESS TECHNOLOGY, INC. 109 Bonaventura Drive, San Jose, CA 95134, USA Tel (408) 643-8800 • WWW.ESSTECH.COM 94 
Register 244: SOFT RAMP DOWN READ 
Bits [7:2] [1] [0] 
Default - - - 
 
Bits Mnemonic Description 
[7:2] RESERVED N/A 
[1] SS_RAMP_DOWN_CH2 Soft ramped down flag ch2 
[0] SS_RAMP_DOWN_CH1 Soft ramped down flag ch1 
Register 245: INPUT STREAM READBACK 
Bits [7:5] [4] [3] [2] [1:0] 
Default - - - - - 
 
Bits Mnemonic Description 
[7:5] RESERVED N/A 
[4] SPDIF_VALID S/PDIF valid flag 
[3] TDM_DATA_VALID TDM valid data flag 
[2] DOP_VALID DoP valid flag 
[1:0] INPUT_SELECT_OVERRIDE AUTO_INPUT_SEL value 
• 2'd0: PCM (default) 
• 2'd1: DSD 
• 2'd2: DoP 
• 2'd3: S/PDIF 
Register 248-246: PROG COEFF OUT READ 
Bits [23:0] 
Default - 
 
Bits Mnemonic Description 
[23:0] PROG_COEFF_OUT Programmable FIR coefficient readback 
Register 250-249: RESERVED 
Register 251: S/PDIF DATA READ 
Bits [7:0] 
Default - 
 
Bits Mnemonic Description 
[7:0] SPDIF_DATA_READ Contains 1 byte of the S/PDIF payload. 
Byte is controlled by Register 136[4:0] SPDIF_DATA_SEL. 
 
 
```

## PDF-страница 95

```
VERSION 0.2.3 
 
95 ESS TECHNOLOGY, INC. 109 Bonaventura Drive, San Jose, CA 95134, USA Tel (408) 643-8800 • WWW.ESSTECH.COM 
ES9039Q2M Product Datasheet                   
 
ES9039Q2M Reference Schematics 
Typical Application Schematic 
MCLK
AGND_DAC2
SCL/SCLK
CHIP_EN
PCM/DSD/DoP /MQA
GPIO1-8*
AVDD
GND
DAC1B
DAC1
DAC2
DAC2B
ES9039Q2M
Application 
Processor
3.3V
RT1
CLK Input
DATA_CLK
DATA1
DATA2
I2C/SPI
3.3V
DVDD
AGND_DAC1
AVCC_DAC1AVCC_DAC2
3.3V
VCCA
3.3V
Output Stage
(also Exposed pad)
Mode (SW)
SDA/MOSI
ADDR1/SS
ADDR0/MISO
8
Hi/Lo (SPI/I2C)
 
Figure 30 - Typical ES9039Q2M Software Mode Application Diagram 
Note: See GPIO section for configuration of GPIOs including GPIO8. 
```

## PDF-страница 96

```
VERSION 0.2.3 
 
ES9039Q2M Product Datasheet 
ESS TECHNOLOGY, INC. 109 Bonaventura Drive, San Jose, CA 95134, USA Tel (408) 643-8800 • WWW.ESSTECH.COM 96 
Hardware (HW) Mode 
 
Figure 31 - Hardware (HW) Mode Reference Schematic for ES9039Q2M 
Note 1: ES9039Q2M has an exposed pad (EPAD, Pin 33) and should be connected to ground. 
Note 2: It is recommended to use Tantalum and COG capacitors (where indicated) to achieve the highest performance. 
 

```

## PDF-страница 97

```
VERSION 0.2.3 
 
97 ESS TECHNOLOGY, INC. 109 Bonaventura Drive, San Jose, CA 95134, USA Tel (408) 643-8800 • WWW.ESSTECH.COM 
ES9039Q2M Product Datasheet                   
 
Software (SW) Mode 
 
Figure 32 - Software Mode Reference Schematic for ES9039Q2M 
Note 1: ES9039Q2M has an exposed pad (EPAD, Pin 33) and should be connected to ground. 
Note 2: It is recommended to use Tantalum and COG capacitors (where indicated) to achieve the highest performance. 
 

```

## PDF-страница 98

```
VERSION 0.2.3 
 
ES9039Q2M Product Datasheet 
ESS TECHNOLOGY, INC. 109 Bonaventura Drive, San Jose, CA 95134, USA Tel (408) 643-8800 • WWW.ESSTECH.COM 98 
Recommended Dual Op-Amp Output Stage 
 
Figure 33 - Dual Op-Amp Output Stage Schematic for ES9039Q2M 
Note 1: Schematic is representative of ES9039Q2M EVB v2.1 
Note 2: C19, C23 & C27 values are chosen specifically for OPA1612, change depending on desired frequency response.  
Note 3: It is recommended to use Tantalum and COG capacitors (where indicated) to achieve the highest performance. 
 

```

## PDF-страница 99

```
VERSION 0.2.3 
 
99 ESS TECHNOLOGY, INC. 109 Bonaventura Drive, San Jose, CA 95134, USA Tel (408) 643-8800 • WWW.ESSTECH.COM 
ES9039Q2M Product Datasheet                   
 
Recommended Triple Op-Amp Output Stage 
 
Figure 34 - Triple Op-Amp Output Stage Schematic for ES9039Q2M 
Note 1: For more information about the triple op-amp output stage, ask your distributor or FAE for the Output Stage 
Topologies Application Note. 
Note 2: It is recommended to use Tantalum and COG capacitors (where indicated) to achieve the highest performance. 
 

```
