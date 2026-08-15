#include "DSP28x_Project.h"
#include <math.h>
#include "grid_pll.h"
#include "statcom_config.h"

#define GRID_TWO_PI                    (6.283185307179586F)
#define GRID_Q_ERROR_FILTER_ALPHA      (0.01F)
#define GRID_FREQUENCY_FILTER_ALPHA    (0.001F)

typedef struct
{
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float x1;
    float x2;
    float y1;
    float y2;
} GridPllBiquad;

typedef struct
{
    float input[3];
    float alpha[3];
    float beta[3];
    float osgB0;
    float osgB2;
    float osgA1;
    float osgA2;
    float osgQb0;
    float osgQb1;
    float loopIntegrator;
    float theta;
    float sine;
    float cosine;
    float qErrorAverage;
    float frequencyFiltered;
    GridPllBiquad qNotch[3];
    Uint16 lockCounter;
    Uint16 unlockCounter;
    Uint16 previousInputValid;
} GridPllState;

volatile float g_gridPllInputPu = 0.0F;
volatile float g_gridPllAlphaPu = 0.0F;
volatile float g_gridPllBetaPu = 0.0F;
volatile float g_gridPllQErrorPu = 0.0F;
volatile float g_gridPllAmplitudePu = 0.0F;
volatile float g_gridPllThetaRad = 0.0F;
volatile float g_gridPllSine = 0.0F;
volatile float g_gridPllCosine = 1.0F;
volatile float g_gridPllFrequencyHz = GRID_PLL_NOMINAL_FREQUENCY_HZ;
volatile Uint16 g_gridPllSignalValid = 0U;
volatile Uint16 g_gridPllLocked = 0U;
volatile Uint16 g_gridPllFrequencyLimited = 0U;

static GridPllState g_pll;

static float GridPll_Abs(float value)
{
    return (value < 0.0F) ? -value : value;
}

static void GridPll_BiquadInit(GridPllBiquad *filter,
                               float b0,
                               float b1,
                               float b2,
                               float a1,
                               float a2)
{
    filter->b0 = b0;
    filter->b1 = b1;
    filter->b2 = b2;
    filter->a1 = a1;
    filter->a2 = a2;
    filter->x1 = 0.0F;
    filter->x2 = 0.0F;
    filter->y1 = 0.0F;
    filter->y2 = 0.0F;
}

static float GridPll_BiquadRun(GridPllBiquad *filter, float input)
{
    float output = (filter->b0 * input) +
                   (filter->b1 * filter->x1) +
                   (filter->b2 * filter->x2) -
                   (filter->a1 * filter->y1) -
                   (filter->a2 * filter->y2);

    filter->x2 = filter->x1;
    filter->x1 = input;
    filter->y2 = filter->y1;
    filter->y1 = output;
    return output;
}

static void GridPll_BiquadReset(GridPllBiquad *filter)
{
    filter->x1 = 0.0F;
    filter->x2 = 0.0F;
    filter->y1 = 0.0F;
    filter->y2 = 0.0F;
}

static void GridPll_UpdateOscillator(float omega)
{
    float delta = omega * GRID_SAMPLE_PERIOD_SECONDS;
    float delta2 = delta * delta;
    float sinDelta = delta * (1.0F - (delta2 / 6.0F));
    float cosDelta = 1.0F - (delta2 * 0.5F) +
                     ((delta2 * delta2) / 24.0F);
    float oldSine = g_pll.sine;
    float oldCosine = g_pll.cosine;

    g_pll.sine = (oldSine * cosDelta) + (oldCosine * sinDelta);
    g_pll.cosine = (oldCosine * cosDelta) - (oldSine * sinDelta);
}

void GridPll_Init(void)
{
    Uint16 index;
    float omega = GRID_TWO_PI * GRID_PLL_NOMINAL_FREQUENCY_HZ;
    float x = 2.0F * GRID_PLL_SOGI_K * omega *
              GRID_SAMPLE_PERIOD_SECONDS;
    float y = omega * GRID_SAMPLE_PERIOD_SECONDS;
    float denominator;

    y *= y;
    denominator = x + y + 4.0F;
    g_pll.osgB0 = x / denominator;
    g_pll.osgB2 = -g_pll.osgB0;
    g_pll.osgA1 = (2.0F * (4.0F - y)) / denominator;
    g_pll.osgA2 = (x - y - 4.0F) / denominator;
    g_pll.osgQb0 = (GRID_PLL_SOGI_K * y) / denominator;
    g_pll.osgQb1 = 2.0F * g_pll.osgQb0;

    for (index = 0U; index < 3U; ++index)
    {
        g_pll.input[index] = 0.0F;
        g_pll.alpha[index] = 0.0F;
        g_pll.beta[index] = 0.0F;
    }

    g_pll.loopIntegrator = 0.0F;
    g_pll.theta = 0.0F;
    g_pll.sine = 0.0F;
    g_pll.cosine = 1.0F;
    g_pll.qErrorAverage = 0.0F;
    g_pll.frequencyFiltered = GRID_PLL_NOMINAL_FREQUENCY_HZ;
    g_pll.lockCounter = 0U;
    g_pll.unlockCounter = 0U;
    g_pll.previousInputValid = 0U;

    /* Park-domain traps reject the dominant terms produced by half-wave
     * even harmonics without changing the DC phase-error component. */
    GridPll_BiquadInit(&g_pll.qNotch[0],
                       0.9922075407F, -1.9841702690F, 0.9922075407F,
                       -1.9841702690F, 0.9844150813F);
    GridPll_BiquadInit(&g_pll.qNotch[1],
                       0.9769887635F, -1.9518083676F, 0.9769887635F,
                       -1.9518083676F, 0.9539775270F);
    GridPll_BiquadInit(&g_pll.qNotch[2],
                       0.9622513159F, -1.9185700325F, 0.9622513159F,
                       -1.9185700325F, 0.9245026319F);

    g_gridPllInputPu = 0.0F;
    g_gridPllAlphaPu = 0.0F;
    g_gridPllBetaPu = 0.0F;
    g_gridPllQErrorPu = 0.0F;
    g_gridPllAmplitudePu = 0.0F;
    g_gridPllThetaRad = 0.0F;
    g_gridPllSine = 0.0F;
    g_gridPllCosine = 1.0F;
    g_gridPllFrequencyHz = GRID_PLL_NOMINAL_FREQUENCY_HZ;
    g_gridPllSignalValid = 0U;
    g_gridPllLocked = 0U;
    g_gridPllFrequencyLimited = 0U;
}

void GridPll_Run(float inputPu, Uint16 inputSignalValid)
{
    float alpha;
    float beta;
    float amplitude;
    float qError;
    float normalizedQ;
    float phaseError;
    float naturalOmega = GRID_TWO_PI * GRID_PLL_LOOP_BANDWIDTH_HZ;
    float kp = 2.0F * GRID_PLL_DAMPING * naturalOmega;
    float ki = naturalOmega * naturalOmega;
    float nominalOmega = GRID_TWO_PI * GRID_PLL_NOMINAL_FREQUENCY_HZ;
    float minOmega = GRID_TWO_PI * GRID_PLL_TRACK_MIN_FREQUENCY_HZ;
    float maxOmega = GRID_TWO_PI * GRID_PLL_TRACK_MAX_FREQUENCY_HZ;
    float integratorCandidate;
    float omega;
    Uint16 valid;
    Uint16 trackingLimited = 0U;
    Uint16 rangeLimited;

    g_pll.input[0] = inputPu;
    alpha = (g_pll.osgB0 * g_pll.input[0]) +
            (g_pll.osgB2 * g_pll.input[2]) +
            (g_pll.osgA1 * g_pll.alpha[1]) +
            (g_pll.osgA2 * g_pll.alpha[2]);
    beta = (g_pll.osgQb0 * g_pll.input[0]) +
           (g_pll.osgQb1 * g_pll.input[1]) +
           (g_pll.osgQb0 * g_pll.input[2]) +
           (g_pll.osgA1 * g_pll.beta[1]) +
           (g_pll.osgA2 * g_pll.beta[2]);

    g_pll.input[2] = g_pll.input[1];
    g_pll.input[1] = g_pll.input[0];
    g_pll.alpha[2] = g_pll.alpha[1];
    g_pll.alpha[1] = alpha;
    g_pll.beta[2] = g_pll.beta[1];
    g_pll.beta[1] = beta;

    amplitude = sqrtf((alpha * alpha) + (beta * beta));
    qError = (g_pll.cosine * alpha) + (g_pll.sine * beta);
    normalizedQ = qError /
                  ((amplitude > GRID_PLL_NORMALIZATION_FLOOR_PU) ?
                   amplitude : GRID_PLL_NORMALIZATION_FLOOR_PU);
    valid = ((inputSignalValid != 0U) &&
             (amplitude >= GRID_PLL_SIGNAL_VALID_AMPLITUDE_PU)) ? 1U : 0U;
    if (valid != 0U)
    {
        if (g_pll.previousInputValid == 0U)
        {
            GridPll_BiquadReset(&g_pll.qNotch[0]);
            GridPll_BiquadReset(&g_pll.qNotch[1]);
            GridPll_BiquadReset(&g_pll.qNotch[2]);
            g_pll.loopIntegrator = 0.0F;
            g_pll.qErrorAverage = 0.0F;
        }
        phaseError = GridPll_BiquadRun(&g_pll.qNotch[0], normalizedQ);
        phaseError = GridPll_BiquadRun(&g_pll.qNotch[1], phaseError);
        phaseError = GridPll_BiquadRun(&g_pll.qNotch[2], phaseError);
    }
    else
    {
        phaseError = 0.0F;
        g_pll.loopIntegrator = 0.0F;
    }
    g_pll.previousInputValid = valid;

    integratorCandidate = g_pll.loopIntegrator +
                          (ki * GRID_SAMPLE_PERIOD_SECONDS * phaseError);
    omega = nominalOmega + (kp * phaseError) + integratorCandidate;

    if (omega > maxOmega)
    {
        omega = maxOmega;
        trackingLimited = 1U;
        if (phaseError < 0.0F)
        {
            g_pll.loopIntegrator = integratorCandidate;
        }
    }
    else if (omega < minOmega)
    {
        omega = minOmega;
        trackingLimited = 1U;
        if (phaseError > 0.0F)
        {
            g_pll.loopIntegrator = integratorCandidate;
        }
    }
    else
    {
        g_pll.loopIntegrator = integratorCandidate;
    }

    g_pll.theta += omega * GRID_SAMPLE_PERIOD_SECONDS;
    if (g_pll.theta >= GRID_TWO_PI)
    {
        g_pll.theta -= GRID_TWO_PI;
    }
    else if (g_pll.theta < 0.0F)
    {
        g_pll.theta += GRID_TWO_PI;
    }
    GridPll_UpdateOscillator(omega);

    g_pll.frequencyFiltered += GRID_FREQUENCY_FILTER_ALPHA *
        ((omega / GRID_TWO_PI) - g_pll.frequencyFiltered);
    rangeLimited = ((g_pll.frequencyFiltered <
                     GRID_PLL_MIN_FREQUENCY_HZ) ||
                    (g_pll.frequencyFiltered >
                     GRID_PLL_MAX_FREQUENCY_HZ)) ? 1U : 0U;

    g_pll.qErrorAverage += GRID_Q_ERROR_FILTER_ALPHA *
                           (GridPll_Abs(phaseError) -
                            g_pll.qErrorAverage);
    if ((valid != 0U) && (rangeLimited == 0U) &&
        (g_pll.qErrorAverage <= GRID_PLL_LOCK_Q_ERROR_PU))
    {
        if (g_pll.lockCounter < GRID_PLL_LOCK_SAMPLES)
        {
            ++g_pll.lockCounter;
        }
        if (g_pll.lockCounter >= GRID_PLL_LOCK_SAMPLES)
        {
            g_gridPllLocked = 1U;
        }
    }
    else
    {
        g_pll.lockCounter = 0U;
    }

    if ((valid == 0U) || (rangeLimited != 0U) ||
        (g_pll.qErrorAverage >= GRID_PLL_UNLOCK_Q_ERROR_PU))
    {
        if (g_pll.unlockCounter < GRID_PLL_UNLOCK_SAMPLES)
        {
            ++g_pll.unlockCounter;
        }
        if ((valid == 0U) ||
            (g_pll.unlockCounter >= GRID_PLL_UNLOCK_SAMPLES))
        {
            g_gridPllLocked = 0U;
        }
    }
    else
    {
        g_pll.unlockCounter = 0U;
    }

    g_gridPllInputPu = inputPu;
    g_gridPllAlphaPu = alpha;
    g_gridPllBetaPu = beta;
    g_gridPllQErrorPu = phaseError;
    g_gridPllAmplitudePu = amplitude;
    g_gridPllThetaRad = g_pll.theta;
    g_gridPllSine = g_pll.sine;
    g_gridPllCosine = g_pll.cosine;
    g_gridPllFrequencyHz = g_pll.frequencyFiltered;
    g_gridPllSignalValid = valid;
    g_gridPllFrequencyLimited = (rangeLimited | trackingLimited);
}

void GridPll_GetStatus(GridPllStatus *status)
{
    status->inputPu = g_gridPllInputPu;
    status->alphaPu = g_gridPllAlphaPu;
    status->betaPu = g_gridPllBetaPu;
    status->qErrorPu = g_gridPllQErrorPu;
    status->fundamentalAmplitudePu = g_gridPllAmplitudePu;
    status->thetaRad = g_gridPllThetaRad;
    status->sine = g_gridPllSine;
    status->cosine = g_gridPllCosine;
    status->frequencyHz = g_gridPllFrequencyHz;
    status->signalValid = g_gridPllSignalValid;
    status->locked = g_gridPllLocked;
    status->frequencyLimited = g_gridPllFrequencyLimited;
}
