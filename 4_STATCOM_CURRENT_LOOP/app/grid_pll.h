#ifndef GRID_PLL_H
#define GRID_PLL_H

#include "F2806x_Device.h"

typedef struct
{
    float inputPu;
    float alphaPu;
    float betaPu;
    float qErrorPu;
    float fundamentalAmplitudePu;
    float thetaRad;
    float sine;
    float cosine;
    float frequencyHz;
    Uint16 signalValid;
    Uint16 locked;
    Uint16 frequencyLimited;
} GridPllStatus;

extern volatile float g_gridPllInputPu;
extern volatile float g_gridPllAlphaPu;
extern volatile float g_gridPllBetaPu;
extern volatile float g_gridPllQErrorPu;
extern volatile float g_gridPllAmplitudePu;
extern volatile float g_gridPllThetaRad;
extern volatile float g_gridPllSine;
extern volatile float g_gridPllCosine;
extern volatile float g_gridPllFrequencyHz;
extern volatile Uint16 g_gridPllSignalValid;
extern volatile Uint16 g_gridPllLocked;
extern volatile Uint16 g_gridPllFrequencyLimited;

void GridPll_Init(void);
void GridPll_Run(float inputPu, Uint16 inputSignalValid);
void GridPll_GetStatus(GridPllStatus *status);

#endif /* GRID_PLL_H */
