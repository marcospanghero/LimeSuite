#include "lms7002m_calibrations.h"
#include "LMS7002M_parameters_compact.h"
#include "spi.h"
#include "lms7002m_controls.h"
#include <math.h>

#ifdef __cplusplus
    #define VERBOSE 1
    //#define DRAW_GNU_PLOTS

    #include <thread>
    #include <vector>
    #include <chrono>
    #include <stdio.h>

    typedef struct
    {
        int16_t value;
        uint32_t signalLevel;
        std::vector<float> measurements;
    } BinSearchResults;
    BinSearchResults results;

    #include <gnuPlotPipe.h>
    GNUPlotPipe saturationPlot;
    GNUPlotPipe rxDCPlot;
    GNUPlotPipe IQImbalancePlot;
    GNUPlotPipe txDCPlot;

///APPROXIMATE conversion
float ChipRSSI_2_dBFS(uint32_t rssi)
{
    uint32_t maxRSSI = 0x15FF4;
    if(rssi == 0)
        rssi = 1;
    return 20*log10((float)(rssi)/maxRSSI);
}

int16_t toSigned(int16_t val, uint8_t msblsb)
{
    val <<= 15-((msblsb >> 4) & 0xF);
    val >>= 15-((msblsb >> 4) & 0xF);
    return val;
}
#else
    #define VERBOSE 0
#endif // __cplusplus

float bandwidthRF = 5e6; //Calibration bandwidth
#define calibrationSXOffset_Hz 1e6
#define offsetNCO 0.1e6
#define calibUserBwDivider 5

ROM uint16_t addr[1];
xdata uint16_t regStore[200];
void BackupRegisters()
{
	/*uint8_t i;
	for(i=0; i<255; ++i)
		regStore[i] = SPI_read(addr[i]); */
}

void RestoreRegisters()
{
	/*uint8_t i;
	for(i=0; i<255; ++i)
		SPI_write(addr[i], regStore[i]); */

}

void SetDefaultsSX()
{
	ROM const uint16_t SXAddr[]=	{0x011C, 0x011D, 0x011E, 0x011F, 0x0120, 0x0121, 0x0122, 0x0123};
	ROM const uint16_t SXdefVals[]={0xAD43, 0x0400, 0x0780, 0x3640, 0xB9FF, 0x3404, 0x033F, 0x067B};

	uint8_t i;
    for(i=0; i<sizeof(SXAddr)/sizeof(uint16_t); ++i)
        SPI_write(SXAddr[i], SXdefVals[i]);
}

uint8_t toDCOffset(const int8_t offset)
{
    return offset >= 0 ? offset : (abs(offset) | 0x40);
}

void FlipRisingEdge(const uint16_t addr, const uint8_t bits)
{
    const uint16_t regVal = SPI_read(addr);
    Modify_SPI_Reg_bits_WrOnly(addr, bits, 0, regVal);
    Modify_SPI_Reg_bits_WrOnly(addr, bits, 1, regVal);
}

void LoadDC_REG_TX_IQ()
{
    SPI_write(0x020C, 0x7FFF);
    FlipRisingEdge(TSGDCLDI_TXTSP);
    SPI_write(0x020C, 0x8000);
    FlipRisingEdge(TSGDCLDQ_TXTSP);
}

static void Delay()
{
#ifdef __cplusplus
    //std::this_thread::sleep_for(std::chrono::milliseconds(10));
#else
	uint16_t i;
	volatile uint8_t t=0;
	for(i=0; i<200; ++i)
		t <<= 1;
#endif
}

#ifdef __cplusplus
uint32_t rssiCounter = 0;
#endif
uint16_t GetRSSI()
{
#ifdef __cplusplus
    ++rssiCounter;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
#else
    #warning TODO: might need to wait until RSSI values refreshes
    Delay();
#endif // __cplusplus
Delay();
Delay();
    FlipRisingEdge(CAPTURE);
    return ((SPI_read(0x040F) << 2) | (SPI_read(0x040E) & 0x3));
}

void SetRxGFIR3Coefficients()
{
    //FIR coefficients symmetrical, storing only one half
    ROM const int16_t firCoefs[] =
{
    8,4,0,-6,-11,-16,-20,-22,-22,-20,-14,-5,6,20,34,46,
    56,61,58,48,29,3,-29,-63,-96,-123,-140,-142,-128,-94,-44,20,
    93,167,232,280,302,291,244,159,41,-102,-258,-409,-539,-628,-658,-614,
    -486,-269,34,413,852,1328,1814,2280,2697,3038,3277,3401,
};
    uint8_t index = 0;
    for (;index < sizeof(firCoefs)/sizeof(int16_t); ++index)
        SPI_write(0x0500 + index + 24 * (index / 40), firCoefs[index]);
    for (;index < sizeof(firCoefs)/sizeof(int16_t)*2; ++index)
        SPI_write(0x0500 + index + 24 * (index / 40), firCoefs[119-index]);
}

void CheckSaturationTxRx()
{
	const uint16_t saturationLevel = 0x0B000; //-3dBFS
	int8_t g_pga;
	int8_t g_rxloop;
	uint16_t rssi_prev;
	uint16_t rssi;
#ifdef DRAW_GNU_PLOTS
    #define PUSH_PLOT_VALUE(vec, data) vec.push_back(data)
    std::vector<float> g_rxLoopbStage;
    std::vector<float> pgaFirstStage;
    std::vector<float> lnaStage;
    std::vector<float> tiaStage;
    std::vector<float> pgaSecondStage;
#else
    #define PUSH_PLOT_VALUE(vec, data)
#endif
    Modify_SPI_Reg_bits(DC_BYP_RXTSP, 0);
    Modify_SPI_Reg_bits(CMIX_BYP_RXTSP, 0);
    SetNCOFrequency(LMS7002M_Rx, calibrationSXOffset_Hz - offsetNCO + (bandwidthRF / calibUserBwDivider) * 2);

    rssi_prev = rssi = GetRSSI();
    PUSH_PLOT_VALUE(g_rxLoopbStage, rssi);

    g_pga = Get_SPI_Reg_bits(G_PGA_RBB);
    g_rxloop = Get_SPI_Reg_bits(G_RXLOOPB_RFE);

#if VERBOSE
    printf("Receiver saturation search, target level: %i (%2.3f dBFS)\n", saturationLevel, ChipRSSI_2_dBFS(saturationLevel));
    printf("initial  PGA: %2i, RxLoopback: %2i, %3.2f dbFS\n", g_pga, g_rxloop, ChipRSSI_2_dBFS(rssi));
#endif
    while(rssi < saturationLevel)
    {
        if(g_rxloop < 15)
            ++g_rxloop;
        else
            break;
        Modify_SPI_Reg_bits(G_RXLOOPB_RFE, g_rxloop);
        rssi = GetRSSI();
        PUSH_PLOT_VALUE(g_rxLoopbStage, rssi);
    }
    PUSH_PLOT_VALUE(pgaFirstStage, rssi);
    while(g_rxloop == 15 && rssi < saturationLevel)
    {
        if(g_pga < 18)
            ++g_pga;
        else
            break;
        Modify_SPI_Reg_bits(G_PGA_RBB, g_pga);
        rssi = GetRSSI();
        if(rssi < rssi_prev)
        {
            --g_pga;
            break;
        }
        rssi_prev = rssi;
        PUSH_PLOT_VALUE(pgaFirstStage, rssi);
    }
#if VERBOSE
    printf("adjusted PGA: %2i, RxLoopback: %2i, %3.2f dbFS\n", g_pga, g_rxloop, ChipRSSI_2_dBFS(rssi));
#endif
    Modify_SPI_Reg_bits(CMIX_BYP_RXTSP, 1);
    Modify_SPI_Reg_bits(DC_BYP_RXTSP, 1);
#ifdef DRAW_GNU_PLOTS
    {
    saturationPlot.write("set yrange [:0]\n");
    saturationPlot.write("set title 'Rx gains search'\n");
    saturationPlot.write("set key right bottom\n");
    saturationPlot.write("set xlabel 'measurement index'\n");
    saturationPlot.write("set ylabel 'RSSI dbFS'\n");
    saturationPlot.write("set grid xtics ytics\n");
    saturationPlot.write("plot\
'-' u 1:2 with lines title 'G_RXLOOPB',\
'-' u 1:2 with lines title 'PGA',\
'-' u 1:2 with lines title 'target Level'\n");
    int index = 1;
    const auto arrays = {&g_rxLoopbStage, &pgaFirstStage};
    for(auto a : arrays)
    {
        --index;
        for(size_t i=0; i<a->size(); ++i)
            saturationPlot.writef("%i %f\n", index++, ChipRSSI_2_dBFS((*a)[i]));
        saturationPlot.write("e\n");
    }
    saturationPlot.writef("%i %f\n%i %f\ne\n", 0, ChipRSSI_2_dBFS(saturationLevel),
                           index, ChipRSSI_2_dBFS(saturationLevel));
    saturationPlot.flush();
    }
#endif
#undef PUSH_PLOT_VALUE
}

typedef struct
{
    LMS7Parameter param;
    int16_t result;
    int16_t minValue;
    int16_t maxValue;
} BinSearchParam;

bool ConvertToRxDC = false;
uint16_t Convert(const uint16_t val)
{
	return ConvertToRxDC ? toDCOffset(val) : val;
}

void BinarySearch(BinSearchParam bdata* args)
{
    uint16_t rssiLeft = ~0;
    uint16_t rssiRight;
    int16_t left = args->minValue;
    int16_t right = args->maxValue;
    int16_t step;
    const uint16_t addr = args->param.address;
    const uint8_t msblsb = args->param.msblsb;
    const uint16_t regValue = SPI_read(addr);
    ConvertToRxDC = args->param.address == DCOFFI_RFE.address;
#ifdef DRAW_GNU_PLOTS
    std::vector<float> searchPoints;
    #define PUSH_PLOT_VALUE(vec, param, level) vec.push_back(param);vec.push_back(level)
#else
    #define PUSH_PLOT_VALUE(vec, param, level)
#endif
    Modify_SPI_Reg_bits_WrOnly(addr, msblsb, Convert(right), regValue);
    rssiRight = GetRSSI();
    PUSH_PLOT_VALUE(searchPoints, right, rssiRight);
    while(right-left >= 1)
    {
        step = (right-left)/2;
        if(rssiLeft < rssiRight)
        {
            Modify_SPI_Reg_bits_WrOnly(addr, msblsb, Convert(right), regValue);
            rssiRight = GetRSSI();
            PUSH_PLOT_VALUE(searchPoints, right, rssiRight);
        }
        else
        {
            Modify_SPI_Reg_bits_WrOnly(addr, msblsb, Convert(left), regValue);
            rssiLeft = GetRSSI();
            PUSH_PLOT_VALUE(searchPoints, left, rssiLeft);
        }
        if(step == 0)
            break;
        if(rssiLeft < rssiRight)
            right -= step;
        else
            left += step;
    }
    args->result = rssiLeft < rssiRight ? left : right;
    Modify_SPI_Reg_bits_WrOnly(addr, msblsb, Convert(args->result), regValue);

#ifdef DRAW_GNU_PLOTS
	results.value = args->result;
    results.signalLevel = rssiLeft < rssiRight ? rssiLeft : rssiRight;
    results.measurements.clear();
    results.measurements = searchPoints;
    for(size_t i=0; i<results.measurements.size(); i+=2)
        for(size_t j=i; j<results.measurements.size(); j+=2)
    {
        if(results.measurements[i] > results.measurements[j])
        {
            float temp = results.measurements[i];
            results.measurements[i] = results.measurements[j];
            results.measurements[j] = temp;
            temp = results.measurements[i+1];
            results.measurements[i+1] = results.measurements[j+1];
            results.measurements[j+1] = temp;
        }
    }
#endif //DRAW_GNU_PLOTS
#undef PUSH_PLOT_VALUE
}

void CalibrateRxDC()
{
#ifdef DRAW_GNU_PLOTS
    rxDCPlot.write("set title 'Rx DC search'\n");
    rxDCPlot.write("set xlabel 'Offset value'\n");
    rxDCPlot.write("set ylabel 'RSSI dBFS'\n");
    rxDCPlot.write("set grid ytics xtics\n");
    rxDCPlot.write("plot\
'-' w l t 'I1',\
'-' w l t 'Q1',\
'-' w l t 'I2',\
'-' w l t 'Q2',\
'-' w l t 'I3',\n");
#endif // DRAW_GNU_PLOTS
    BinSearchParam argsI;
    BinSearchParam argsQ;

	ConvertToRxDC = true;
    Modify_SPI_Reg_bits(EN_G_TRF, 0);
    Modify_SPI_Reg_bits(DC_BYP_RXTSP, 1);
    Modify_SPI_Reg_bits(CAPSEL, 0);
    //SetRxDCOFF(32, 32);
    SPI_write(0x010E, toDCOffset(32) << 7 | toDCOffset((32)));
	//return;
    //find I
    argsI.param = DCOFFI_RFE;
    argsQ.param = DCOFFQ_RFE;
    argsI.maxValue = argsQ.maxValue = 63;
    argsI.minValue = argsQ.minValue = -63;

    BinarySearch(&argsI);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        rxDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    rxDCPlot.write("e\n");
#endif
    BinarySearch(&argsQ);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        rxDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    rxDCPlot.write("e\n");
#endif
    argsI.maxValue = argsI.result+8;
    argsI.minValue = argsI.result-8;
    argsQ.maxValue = argsQ.result+8;
    argsQ.minValue = argsQ.result-8;

    BinarySearch(&argsI);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        rxDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    rxDCPlot.write("e\n");
#endif
    BinarySearch(&argsQ);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        rxDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    rxDCPlot.write("e\n");
#endif

    argsI.maxValue = argsI.result+4;
    argsI.minValue = argsI.result-4;
    BinarySearch(&argsI);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        rxDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    rxDCPlot.write("e\n");
    rxDCPlot.flush();
#endif
    Modify_SPI_Reg_bits(DC_BYP_RXTSP, 0); // DC_BYP 0
    Modify_SPI_Reg_bits(EN_G_TRF, 1);
#if VERBOSE
    printf("Rx DCOFFI: %i, DCOFFQ: %i\n", argsI.result, argsQ.result);
#endif
	ConvertToRxDC = false;
}

void CalibrateTxDC()
{
#ifdef DRAW_GNU_PLOTS
    txDCPlot.write("set title 'Tx DC search'\n");
    txDCPlot.write("set xlabel 'Offset value'\n");
    txDCPlot.write("set ylabel 'RSSI dBFS'\n");
    txDCPlot.write("set grid ytics xtics\n");
    txDCPlot.write("plot\
'-' w l t 'I1',\
'-' w l t 'Q1',\
'-' w l t 'I2',\
'-' w l t 'Q2'\n");
#endif // DRAW_GNU_PLOTS
    BinSearchParam argsI;
    BinSearchParam argsQ;

    Modify_SPI_Reg_bits(EN_G_TRF, 1);
    Modify_SPI_Reg_bits(CMIX_BYP_TXTSP, 0);
    Modify_SPI_Reg_bits(CMIX_BYP_RXTSP, 0);

    //Modify_SPI_Reg_bits(DCCORRI_TXTSP, 0);
    //Modify_SPI_Reg_bits(DCCORRQ_TXTSP, 0);
    Modify_SPI_Reg_bits(0x0204, 15<< 4 | 0, 0);

    //find I
    argsI.param = DCCORRI_TXTSP;
    argsI.maxValue = 127;
    argsI.minValue = -128;
    BinarySearch(&argsI);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        txDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    txDCPlot.write("e\n");
#endif

    argsQ.param = DCCORRQ_TXTSP;
    argsQ.maxValue = 127;
    argsQ.minValue = -128;

    BinarySearch(&argsQ);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        txDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    txDCPlot.write("e\n");
#endif
    //Modify_SPI_Reg_bits(DCCORRQ_TXTSP, corrQ);

    argsI.maxValue = argsI.result+4;
    argsI.minValue = argsI.result-4;
    BinarySearch(&argsI);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        txDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    txDCPlot.write("e\n");
#endif

    argsQ.maxValue = argsQ.result+4;
    argsQ.minValue = argsQ.result-4;
    BinarySearch(&argsQ);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        txDCPlot.writef("%f %f\n", results.measurements[i], results.measurements[i+1]);
    txDCPlot.write("e\n");
#endif
#if VERBOSE
    printf("coarse: Tx DCCORRI: %i, DCCORRQ: %i | %2.3f dBFS\n", argsI.result, argsQ.result, ChipRSSI_2_dBFS(results.signalLevel));
#endif
    Modify_SPI_Reg_bits(0x0204, 15 << 4 | 0, (argsI.result << 8 | (argsQ.result & 0xFF)));
}

void CalibrateIQImbalance(bool tx)
{
#ifdef DRAW_GNU_PLOTS
    const char *dirName = tx ? "Tx" : "Rx";
    IQImbalancePlot.writef("set title '%s IQ imbalance'\n", dirName);
    IQImbalancePlot.write("set xlabel 'parameter value'\n");
    IQImbalancePlot.write("set ylabel 'RSSI dBFS'\n");
    IQImbalancePlot.write("set grid ytics xtics\n");
    IQImbalancePlot.write("plot\
'-' w l t 'phase1',\
'-' w l t 'gain1',\
'-' w l t 'phase2'\
\n");
#endif // DRAW_GNU_PLOTS
    LMS7Parameter gcorri;
    LMS7Parameter gcorrq;
    BinSearchParam argsPhase;
    BinSearchParam argsGain;

	if(tx)
	{
		gcorri = GCORRI_TXTSP;
		gcorrq = GCORRQ_TXTSP;
		argsPhase.param = IQCORR_TXTSP;
	}
	else
	{
		gcorri = GCORRI_RXTSP;
		gcorrq = GCORRQ_RXTSP;
		argsPhase.param = IQCORR_RXTSP;
	}

   	argsPhase.maxValue = 128;
    argsPhase.minValue = -128;
    BinarySearch(&argsPhase);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        IQImbalancePlot.writef("%f %f\n", results.measurements[i], ChipRSSI_2_dBFS(results.measurements[i+1]));
    IQImbalancePlot.write("e\n");
    printf("Coarse search %s IQCORR: %i\n", dirName, argsPhase.result);
#endif

    //coarse gain
    {
        uint16_t rssiIgain;
        uint16_t rssiQgain;
        //Modify_SPI_Reg_bits(gcorri.address, gcorri.msblsb, 2047 - 64);
        //Modify_SPI_Reg_bits(gcorrq.address, gcorrq.msblsb, 2047);
        SPI_write(gcorri.address, 2047 - 64);
        SPI_write(gcorrq.address, 2047);
        rssiIgain = GetRSSI();
        //Modify_SPI_Reg_bits(gcorri.address, gcorri.msblsb, 2047);
        //Modify_SPI_Reg_bits(gcorrq.address, gcorrq.msblsb, 2047 - 64);
        SPI_write(gcorri.address, 2047);
        SPI_write(gcorrq.address, 2047 - 64);
        rssiQgain = GetRSSI();

        if(rssiIgain < rssiQgain)
            argsGain.param = gcorri;
        else
            argsGain.param = gcorrq;
        SPI_write(gcorrq.address, 2047);
    }
    argsGain.maxValue = 2047;
    argsGain.minValue = 2047-512;
    BinarySearch(&argsGain);

#ifdef DRAW_GNU_PLOTS
    const char* chName = (argsGain.param.address == gcorri.address ? "I" : "Q");
    for(size_t i=0; i<results.measurements.size(); i+=2)
        IQImbalancePlot.writef("%f %f\n", results.measurements[i], ChipRSSI_2_dBFS(results.measurements[i+1]));
    IQImbalancePlot.write("e\n");
    printf("Coarse search %s GAIN_%s: %i\n", dirName, chName, argsGain.result);
#endif

    argsPhase.maxValue = argsPhase.result+16;
    argsPhase.minValue = argsPhase.result-16;
    BinarySearch(&argsPhase);
#ifdef DRAW_GNU_PLOTS
    for(size_t i=0; i<results.measurements.size(); i+=2)
        IQImbalancePlot.writef("%f %f\n", results.measurements[i], ChipRSSI_2_dBFS(results.measurements[i+1]));
    IQImbalancePlot.write("e\n");
    printf("Coarse search %s IQCORR: %i\n", dirName, argsPhase.result);
#endif
    //Modify_SPI_Reg_bits(argsGain.param.address, argsGain.param.msblsb, argsGain.result);
    SPI_write(argsGain.param.address, argsGain.result);
    Modify_SPI_Reg_bits(argsPhase.param.address, argsPhase.param.msblsb, argsPhase.result);
}

uint8_t SetupCGEN()
{
    uint8_t status;
    uint8_t cgenMultiplier;
    uint8_t gfir3n;
    cgenMultiplier = (int)((GetFrequencyCGEN() / 46.08e6) + 0.5);
    if(cgenMultiplier < 2)
        cgenMultiplier = 2;
    if(cgenMultiplier > 9 && cgenMultiplier < 12)
        cgenMultiplier = 12;
    if(cgenMultiplier > 13)
        cgenMultiplier = 13;
    //CGEN VCO is powered up in SetFrequencyCGEN/Tune
    status = SetFrequencyCGEN(46.08e6 * cgenMultiplier);
    if(status != 0)
        return status;

    gfir3n = 4 * cgenMultiplier;
    if(Get_SPI_Reg_bits(EN_ADCCLKH_CLKGN) == 1)
        gfir3n /= pow2(Get_SPI_Reg_bits(CLKH_OV_CLKL_CGEN));
    gfir3n = pow2((int)(log(gfir3n)/log(2)))-1; //could be log2(gfir3n)
    Modify_SPI_Reg_bits(GFIR3_N_RXTSP, gfir3n);
    return 0;
}

uint8_t CalibrateTxSetup()
{
	uint8_t status = 1;
    const uint16_t x0020val = SPI_read(0x0020); //remember used channel

    /*BeginBatch("TxSetup");
    //rfe
    //reset RFE to defaults
    SetDefaults(SECTION_RFE);
    Modify_SPI_Reg_bits(G_RXLOOPB_RFE, 7);
    Modify_SPI_Reg_bits(0x010C, 4 << 4 | 3, 0); //PD_MXLOBUF_RFE 0, PD_QGEN_RFE 0
    Modify_SPI_Reg_bits(CCOMP_TIA_RFE, 4);
    Modify_SPI_Reg_bits(CFB_TIA_RFE, 50);
    Modify_SPI_Reg_bits(ICT_LODC_RFE, 31);
    Modify_SPI_Reg_bits(EN_DCOFF_RXFE_RFE, 1);

    //RBB
    //reset RBB to defaults
    SetDefaults(SECTION_RBB);
    Modify_SPI_Reg_bits(PD_LPFH_RBB, 0);
    Modify_SPI_Reg_bits(PD_LPFL_RBB, 1);
    Modify_SPI_Reg_bits(G_PGA_RBB, 0);
    Modify_SPI_Reg_bits(INPUT_CTL_PGA_RBB, 1);
    Modify_SPI_Reg_bits(ICT_PGA_OUT_RBB, 12);
    Modify_SPI_Reg_bits(ICT_PGA_IN_RBB, 12);

    //TXTSP
    Modify_SPI_Reg_bits(TSGMODE_TXTSP, 1);
    Modify_SPI_Reg_bits(INSEL_TXTSP, 1);
    Modify_SPI_Reg_bits(CMIX_BYP_TXTSP, 0);
    Modify_SPI_Reg_bits(DC_BYP_TXTSP, 0);
    Modify_SPI_Reg_bits(GC_BYP_TXTSP, 0);
    Modify_SPI_Reg_bits(PH_BYP_TXTSP, 0);
    Modify_SPI_Reg_bits(GCORRI_TXTSP.address, GCORRI_TXTSP.msblsb , 2047);
    Modify_SPI_Reg_bits(GCORRQ_TXTSP.address, GCORRQ_TXTSP.msblsb, 2047);
    Modify_SPI_Reg_bits(CMIX_SC_TXTSP, 0);

    //RXTSP
    SetDefaults(SECTION_RxTSP);
    SetDefaults(SECTION_RxNCO);
    Modify_SPI_Reg_bits(GFIR2_BYP_RXTSP, 1);
    Modify_SPI_Reg_bits(GFIR1_BYP_RXTSP, 1);
    Modify_SPI_Reg_bits(HBD_OVR_RXTSP, 4); //Decimation HBD ratio



    Modify_SPI_Reg_bits(AGC_MODE_RXTSP, 1);
    Modify_SPI_Reg_bits(CMIX_BYP_RXTSP, 1);
    Modify_SPI_Reg_bits(AGC_AVG_RXTSP, 0x1);
    Modify_SPI_Reg_bits(GFIR3_L_RXTSP, 7);


    //AFE
    Modify_SPI_Reg_bits(PD_RX_AFE1, 0);
    Modify_SPI_Reg_bits(PD_RX_AFE2, 0);

    //XBUF
    Modify_SPI_Reg_bits(0x0085, 2 << 4 | 0, 1); //PD_XBUF_RX 0, PD_XBUF_TX 0, EN_G_XBUF 1

    //CDS
    Modify_SPI_Reg_bits(CDS_TXATSP, 3);
    Modify_SPI_Reg_bits(CDS_TXBTSP, 3);

    //TRF
    Modify_SPI_Reg_bits(L_LOOPB_TXPAD_TRF, 0);
    Modify_SPI_Reg_bits(EN_LOOPB_TXPAD_TRF, 1);

    BIAS
    {
        uint16_t backup = Get_SPI_Reg_bits(RP_CALIB_BIAS);
        SetDefaults(SECTION_BIAS);
        Modify_SPI_Reg_bits(RP_CALIB_BIAS, backup);
    }

    EndBatch();*/

	{
		ROM const uint16_t TxSetupAddr[] = {0x0082,0x0085,0x00AE,0x0101,0x0200,0x0208, 0x0084};
		ROM const uint16_t TxSetupData[] = {0x0000,0x0001,0xF000,0x0001,0x000C,0x0000, 0x0000};
		ROM const uint16_t TxSetupMask[] = {0x0018,0x0007,0xF000,0x1801,0x000C,0x210B, 0xF83F};
		uint8_t i;
 	    for(i=0; i<sizeof(TxSetupAddr)/sizeof(uint16_t); ++i)
        	SPI_write(TxSetupAddr[i], ( SPI_read(TxSetupAddr[i]) & ~TxSetupMask[i] ) | TxSetupData[i]);
	}
	{
		ROM const uint16_t TxSetupAddrWrOnly[] = {0x010C,0x010D,0x010E,0x010F,0x0110,0x0111,0x0112,0x0113,0x0115,0x0116,0x0117,0x0118,0x0119, 0x0201, 0x0202, 0x0400,0x0401,0x0402,0x0403,0x0404,0x0405,0x0406,0x0407,0x0408,0x0409,0x040A,0x040B,0x040C,0x040D,0x040E,0x0440,0x0441,0x0442,0x0443};
		ROM const uint16_t TxSetupDataWrOnly[] = {0x88E5,0x00DE,0x2040,0x3042,0x0BFF,0x0083,0x4032,0x03DF,0x0005,0x8180,0x280C,0x218C,0x3180, 0x07FF, 0x07FF, 0x0081,0x07FF,0x07FF,0x4000,0x0000,0x0000,0x0000,0x0700,0x0000,0x0000,0x1000,0x0000,0x0098,0x0000,0x0002,0x0020,0x0000,0x0000,0x0000};

		uint8_t i;
	    for(i=0; i<sizeof(TxSetupAddrWrOnly)/sizeof(uint16_t); ++i)
	        SPI_write(TxSetupAddrWrOnly[i], TxSetupDataWrOnly[i]);
	}
    SetRxGFIR3Coefficients();
	status = SetupCGEN();
    if(status != 0)
        return status;
   	
    //SXR
    Modify_SPI_Reg_bits_WrOnly(MAC, 1, x0020val); //switch to ch. A
    //SetDefaults(SECTION_SX);
	SetDefaultsSX();
	/*{
		ROM const uint16_t SXAddr[]=	{0x011C, 0x011D, 0x011E, 0x011F, 0x0120, 0x0121, 0x0122, 0x0123};
		ROM const uint16_t SXdefVals[]={0xAD43, 0x0400, 0x0780, 0x3640, 0xB9FF, 0x3404, 0x033F, 0x067B};

		uint8_t i;
	    for(i=0; i<sizeof(SXAddr)/sizeof(uint16_t); ++i)
	        SPI_write(SXAddr[i], SXdefVals[i]);
	}*/
	{
        const float_type SXRfreq = GetFrequencySX(LMS7002M_Tx) - bandwidthRF/ calibUserBwDivider - calibrationSXOffset_Hz;
        //SX VCO is powered up in SetFrequencySX/Tune
        status = SetFrequencySX(LMS7002M_Rx, SXRfreq);
        if(status != 0)
            return status+0x50;
    }
    
    //if calibrating ch. B enable buffers
    if(x0020val & 0x2)
    {
        Modify_SPI_Reg_bits(PD_TX_AFE2, 0);
        Modify_SPI_Reg_bits(EN_NEXTRX_RFE, 1);
        Modify_SPI_Reg_bits(EN_NEXTTX_TRF, 1);
    }

    //SXT{
    Modify_SPI_Reg_bits_WrOnly(MAC, 2, x0020val); //switch to ch. B
    Modify_SPI_Reg_bits(PD_LOCH_T2RBUF, 1);
    SPI_write(0x0020, x0020val); //restore used channel

    LoadDC_REG_TX_IQ();
    SetNCOFrequency(LMS7002M_Tx, bandwidthRF/ calibUserBwDivider);
    {
        const uint8_t sel_band1_2_trf = (uint8_t)Get_SPI_Reg_bits(0x0103, 11 << 4 | 10);
        if(sel_band1_2_trf != 0x1 && sel_band1_2_trf != 0x2) //BAND1
        {
            //printf("Tx Calibration: band not selected");
            return 5;
        }
        Modify_SPI_Reg_bits(SEL_PATH_RFE, sel_band1_2_trf+1);
        //Modify_SPI_Reg_bits(PD_RLOOPB_1_RFE, 0);
        //Modify_SPI_Reg_bits(PD_RLOOPB_2_RFE, 1);
        Modify_SPI_Reg_bits(0x010C, 6 << 4 | 5, sel_band1_2_trf ^ 0x3);
        //Modify_SPI_Reg_bits(EN_INSHSW_LB1_RFE, 0);
        //Modify_SPI_Reg_bits(EN_INSHSW_LB2_RFE, 1);
        Modify_SPI_Reg_bits(0x010D, 4 << 4 | 3, sel_band1_2_trf ^ 0x3);
    }
    return 0x0;
}

uint8_t CalibrateTx()
{
#ifdef __cplusplus
    auto beginTime = std::chrono::high_resolution_clock::now();
#endif
#if VERBOSE
    uint8_t ch = (uint8_t)Get_SPI_Reg_bits(MAC);
    uint8_t sel_band1_trf = (uint8_t)Get_SPI_Reg_bits(SEL_BAND1_TRF);
    printf("Tx ch.%s , BW: %g MHz, RF output: %s, Gain: %i\n",
                    ch == 0x1 ? "A" : "B",
                    bandwidthRF/1e6,
                    sel_band1_trf==1 ? "BAND1" : "BAND2",
                    Get_SPI_Reg_bits(CG_IAMP_TBB));
#endif
    uint8_t status;
	BackupRegisters();
    status = CalibrateTxSetup();
#ifdef __cplusplus
    printf("Setup duration: %li ms\n",
        std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::high_resolution_clock::now()-beginTime).count());
#endif
    if(status != 0)
	{
		goto TxCalibrationEnd; //go to ending stage to restore registers
	}
	CheckSaturationTxRx();
    CalibrateRxDC();

    SetNCOFrequency(LMS7002M_Rx, calibrationSXOffset_Hz - offsetNCO + (bandwidthRF/ calibUserBwDivider));
    CalibrateTxDC();
    SetNCOFrequency(LMS7002M_Rx, calibrationSXOffset_Hz - offsetNCO);
    CalibrateIQImbalance(LMS7002M_Tx);
TxCalibrationEnd:
	if(status != 0)
    {
#if VERBOSE
        printf("Tx calibration failed");
#endif
		RestoreRegisters();
        return status;
    }    
	RestoreRegisters();
    //Modify_SPI_Reg_bits(MAC, ch);
    //Modify_SPI_Reg_bits(DCCORRI_TXTSP.address, DCCORRI_TXTSP.msblsb, dccorri);
    //Modify_SPI_Reg_bits(DCCORRQ_TXTSP.address, DCCORRQ_TXTSP.msblsb, dccorrq);
    //Modify_SPI_Reg_bits(GCORRI_TXTSP.address, GCORRI_TXTSP.msblsb, gcorri);
    //Modify_SPI_Reg_bits(GCORRQ_TXTSP.address, GCORRQ_TXTSP.msblsb, gcorrq);
    //Modify_SPI_Reg_bits(IQCORR_TXTSP.address, IQCORR_TXTSP.msblsb, phaseOffset);

    //Modify_SPI_Reg_bits(DC_BYP_TXTSP, 0);
    //Modify_SPI_Reg_bits(GC_BYP_TXTSP, 0);
    //Modify_SPI_Reg_bits(PH_BYP_TXTSP, 0);
    //Modify_SPI_Reg_bits(0x0208, 3 << 4 | 0, 0);

    //LoadDC_REG_TX_IQ(); //not necessary, just for testing convenience
#if VERBOSE
    //printf("#####Tx calibration RESULTS:###########################\n");
    /*printf("Tx ch.%s, BW: %g MHz, RF output: %s, Gain: %i\n",
                    ch == 1 ? "A" : "B",
                    bandwidthRF/1e6, sel_band1_trf==1 ? "BAND1" : "BAND2",
                    1//Get_SPI_Reg_bits(CG_IAMP_TBB)
                    );*/
    {
        //Get_SPI_Reg_bits(CG_IAMP_TBB);
        int8_t dcI = Get_SPI_Reg_bits(DCCORRI_TXTSP.address, DCCORRI_TXTSP.msblsb);
        int8_t dcIsigned = toSigned(dcI, DCCORRI_TXTSP.msblsb);
    int8_t dcQsigned = toSigned(Get_SPI_Reg_bits(DCCORRQ_TXTSP.address, DCCORRQ_TXTSP.msblsb), DCCORRQ_TXTSP.msblsb);
    int16_t phaseSigned = toSigned(Get_SPI_Reg_bits(IQCORR_TXTSP.address, IQCORR_TXTSP.msblsb), IQCORR_TXTSP.msblsb);
    uint16_t gcorri = Get_SPI_Reg_bits(GCORRI_TXTSP.address, GCORRI_TXTSP.msblsb);
    uint16_t gcorrq = Get_SPI_Reg_bits(GCORRQ_TXTSP.address, GCORRQ_TXTSP.msblsb);
    printf("   | DC  | GAIN | PHASE\n");
    printf("---+-----+------+------\n");
    printf("I: | %3i | %4i | %i\n", dcIsigned, gcorri, phaseSigned);
    printf("Q: | %3i | %4i |\n", dcQsigned, gcorrq);
    }
#ifdef __cplusplus
    int32_t duration = std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::high_resolution_clock::now()-beginTime).count();
    printf("Duration: %i ms\n", duration);
#endif
#endif //LMS_VERBOSE_OUTPUT
    return 0;
}
#define MSBLSB(x, y) x << 4 | y

 /*
void SetDefaults(uint16_t start, uint16_t end)
{
    ROM const uint16_t defaultAddrs[] = { 0
//0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002E,0x002F,0x0081,0x0082,0x0084,0x0085,0x0086,0x0087,0x0088,0x0089,0x008A,0x008B,0x008C,0x0092,0x0093,0x0094,0x0095,0x0096,0x0097,0x0098,0x0099,0x009A,0x009B,0x009C,0x009D,0x009E,0x009F,0x00A0,0x00A1,0x00A2,0x00A3,0x00A4,0x00A5,0x00A6,0x00A7,0x00A8,0x00AA,0x00AB,0x00AD,0x00AE,0x0100,0x0101,0x0102,0x0103,0x0104,0x0105,0x0106,0x0107,0x0108,0x0109,0x010A,0x010C,0x010D,0x010E,0x010F,0x0110,0x0111,0x0112,0x0113,0x0114,0x0115,0x0116,0x0117,0x0118,0x0119,0x011A,0x011C,0x011D,0x011E,0x011F,0x0120,0x0121,0x0122,0x0123,0x0124,0x0200,0x0201,0x0202,0x0203,0x0204,0x0205,0x0206,0x0207,0x0208,0x0209,0x020A,0x020C,0x0240,0x0242,0x0243,0x0244,0x0245,0x0246,0x0247,0x0248,0x0249,0x024A,0x024B,0x024C,0x024D,0x024E,0x024F,0x0250,0x0251,0x0252,0x0253,0x0254,0x0255,0x0256,0x0257,0x0258,0x0259,0x025A,0x025B,0x025C,0x025D,0x025E,0x025F,0x0260,0x0261,0x0400,0x0401,0x0402,0x0403,0x0404,0x0405,0x0406,0x0407,0x0408,0x0409,0x040A,0x040B,0x040C,0x040E,0x0440,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F,0x0450,0x0451,0x0452,0x0453,0x0454,0x0455,0x0456,0x0457,0x0458,0x0459,0x045A,0x045B,0x045C,0x045D,0x045E,0x045F,0x0460,0x0461
};
	ROM const uint16_t defaultValues[] = { 0
//0xFFFF,0x0E9F,0x07DF,0x5559,0xE4E4,0x0101,0x0101,0xE4E4,0x0101,0x0101,0x0086,0x0010,0xFFFF,0x0000,0x3840,0x0000,0x800B,0x0400,0x0001,0x4901,0x0400,0x0780,0x0020,0x0514,0x2100,0x067B,0x0001,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x6565,0x658C,0x6565,0x658C,0x6565,0x658C,0x658C,0x6565,0x6565,0x6565,0x6565,0x6565,0x6565,0x000F,0x6565,0x0000,0x0000,0x0000,0x03FF,0xF000,0x3409,0x7800,0x3180,0x0A12,0x0088,0x0007,0x318C,0x318C,0x9426,0x61C1,0x104C,0x88FD,0x009E,0x2040,0x3042,0x0BF4,0x0083,0xC0E6,0x03C3,0x008D,0x0009,0x8180,0x280C,0x018C,0x18CB,0x2E02,0xAD43,0x0400,0x0780,0x3640,0xB9FF,0x3404,0x033F,0x067B,0x0000,0x0081,0x07FF,0x07FF,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0020,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0081,0x07FF,0x07FF,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0020,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000
};
#ifdef __cplusplus
    int status = 0;
    std::vector<uint16_t> addrs;
    std::vector<uint16_t> values;
    for(uint32_t address = start; address <= end; ++address)
    {
        int i=0;
        for(i=0; i<sizeof(defaultAddrs)/sizeof(uint16_t); ++i)
        {
            addrs.push_back(defaultAddrs[i]);
            values.push_back(defaultValues[i]);
            break;
        }
    }
    SPI_write_batch(&addrs[0], &values[0], addrs.size());
#else

#endif
} */

int CalibrateRxSetup()
{
    uint8_t status;
    const uint16_t x0020val = SPI_read(0x0020);

    //rfe
    {
        ROM const uint16_t RxSetupAddr[] = {0x0082, 0x0085, 0x00AE, 0x0100, 0x0101, 0x0108, 0x010C, 0x010D, 0x0110, 0x0113, 0x0115, 0x0119, 0x0200, 0x0208, 0x0400, 0x0403, 0x0407, 0x040A, 0x040C};
        ROM const uint16_t RxSetupData[] = {0x0000, 0x0001, 0xF000, 0x0000, 0x0001, 0x0426, 0x0000, 0x0040, 0x001F, 0x000C, 0x0000, 0x0000, 0x008C, 0x2070, 0x0000, 0x4000, 0x0700, 0x1000, 0x0098};
        ROM const uint16_t RxSetupMask[] = {0x0008, 0x0007, 0xF000, 0x0001, 0x1801, 0xFFFF, 0x001A, 0x0040, 0x001F, 0x003C, 0xC000, 0x8000, 0x018C, 0xE170, 0x6000, 0x7000, 0x0700, 0x3007, 0xC0D8};

		uint8_t i;
 	    for(i=0; i<sizeof(RxSetupAddr)/sizeof(uint16_t); ++i)
        	SPI_write(RxSetupAddr[i], ( SPI_read(RxSetupAddr[i]) & ~RxSetupMask[i] ) | RxSetupData[i]);																  	
    }
   /* BeginBatch("RxSetup.txt");
    Modify_SPI_Reg_bits(EN_DCOFF_RXFE_RFE, 1);
    Modify_SPI_Reg_bits(G_RXLOOPB_RFE, 3);
    Modify_SPI_Reg_bits(0x010C, 4 << 4 | 3, 0); //PD_MXLOBUF_RFE 0, PD_QGEN_RFE 0
    Modify_SPI_Reg_bits(0x010C, 1 << 4 | 1, 0); //PD_TIA 0
    Modify_SPI_Reg_bits(0x0110, 4 << 4 | 0, 31); //ICT_LO_RFE 31

    //RBB
    Modify_SPI_Reg_bits(0x0115, MSBLSB(15, 14), 0); //Loopback switches disable
    Modify_SPI_Reg_bits(0x0119, MSBLSB(15, 15), 0); //OSW_PGA 0

    //TRF
    //reset TRF to defaults
    SetDefaults(SECTION_TRF);
    Modify_SPI_Reg_bits(L_LOOPB_TXPAD_TRF, 0);
    Modify_SPI_Reg_bits(EN_LOOPB_TXPAD_TRF, 1);

    Modify_SPI_Reg_bits(EN_G_TRF, 0);

    //TBB
    //reset TBB to defaults
    SetDefaults(SECTION_TBB);
    Modify_SPI_Reg_bits(CG_IAMP_TBB, 1);
    Modify_SPI_Reg_bits(ICT_IAMP_FRP_TBB, 1);
    Modify_SPI_Reg_bits(ICT_IAMP_GG_FRP_TBB, 6);

    //AFE
    Modify_SPI_Reg_bits(PD_RX_AFE2, 0);

    

    //XBUF
    Modify_SPI_Reg_bits(0x0085, MSBLSB(2, 0), 1); //PD_XBUF_RX 0, PD_XBUF_TX 0, EN_G_XBUF 1

    //TXTSP
    SetDefaults(SECTION_TxTSP);
    SetDefaults(SECTION_TxNCO);
    Modify_SPI_Reg_bits(TSGFCW_TXTSP, 1);
    Modify_SPI_Reg_bits(TSGMODE_TXTSP, 0x1);
    Modify_SPI_Reg_bits(INSEL_TXTSP, 1);
    Modify_SPI_Reg_bits(0x0208, MSBLSB(6, 4), 0x7); //GFIR3_BYP 1, GFIR2_BYP 1, GFIR1_BYP 1
    Modify_SPI_Reg_bits(CMIX_GAIN_TXTSP, 0);
    Modify_SPI_Reg_bits(CMIX_SC_TXTSP, 1);
    Modify_SPI_Reg_bits(CMIX_BYP_TXTSP, 0);

    //RXTSP
    SetDefaults(SECTION_RxTSP);
    SetDefaults(SECTION_RxNCO);
    Modify_SPI_Reg_bits(0x040C, MSBLSB(4, 3), 0x3); //GFIR2_BYP, GFIR1_BYP
    Modify_SPI_Reg_bits(HBD_OVR_RXTSP, 4);

    Modify_SPI_Reg_bits(AGC_MODE_RXTSP, 1);
    Modify_SPI_Reg_bits(AGC_BYP_RXTSP, 0);
    Modify_SPI_Reg_bits(CMIX_BYP_RXTSP, 1);
    Modify_SPI_Reg_bits(CAPSEL, 0);
    Modify_SPI_Reg_bits(AGC_AVG_RXTSP, 0);
    Modify_SPI_Reg_bits(CMIX_GAIN_RXTSP, 0);
    Modify_SPI_Reg_bits(GFIR3_L_RXTSP, 7);

    //CDS
    Modify_SPI_Reg_bits(CDS_TXATSP, 3);
    Modify_SPI_Reg_bits(CDS_TXBTSP, 3);
    EndBatch();	  */
    //BIAS
    Modify_SPI_Reg_bits(AGC_AVG_RXTSP, 0);
    {
        uint16_t rp_calib_bias = Get_SPI_Reg_bits(0x0084, MSBLSB(10, 6));
        //SetDefaults(SECTION_BIAS);
        Modify_SPI_Reg_bits(0x0084, MSBLSB(10, 6), rp_calib_bias);
    }
    {
        uint8_t selPath;
        selPath = Get_SPI_Reg_bits(SEL_PATH_RFE);

        if (selPath == 2)
        {
            Modify_SPI_Reg_bits(SEL_BAND2_TRF, 1);
            Modify_SPI_Reg_bits(SEL_BAND1_TRF, 0);
        }
        else if (selPath == 3)
        {
            Modify_SPI_Reg_bits(SEL_BAND2_TRF, 0);
            Modify_SPI_Reg_bits(SEL_BAND1_TRF, 1);
        }
        else
            return 1;//ReportError(EINVAL, "CalibrateRxSetup() - SEL_PATH_RFE must be LNAL or LNAW");
    }

    Modify_SPI_Reg_bits(MAC, 2);
    if(Get_SPI_Reg_bits(PD_LOCH_T2RBUF) == 0) //isTDD
    {
        //in TDD do nothing
        Modify_SPI_Reg_bits(MAC, 1);
        //SetDefaults(SECTION_SX);
		SetDefaultsSX();
        status = SetFrequencySX(LMS7002M_Rx, GetFrequencySX(LMS7002M_Tx) - bandwidthRF/ calibUserBwDivider - 9e6);
        if(status != 0)
            return status+0x40;
        //done in set frequency/tune
        //Modify_SPI_Reg_bits(PD_VCO, 0);
    }
    else
    {
        float_type SXRfreqHz;
        //SXR
        //Modify_SPI_Reg_bits(MAC, 1); //Get freq already changes/restores ch
        SXRfreqHz = GetFrequencySX(LMS7002M_Rx);

        //SXT
        Modify_SPI_Reg_bits(MAC, 2);
        SetDefaultsSX();

        //done in set frequency/tune
        //Modify_SPI_Reg_bits(PD_VCO, 0);

        status = SetFrequencySX(LMS7002M_Tx, SXRfreqHz + bandwidthRF/ calibUserBwDivider + 9e6);
        if(status != 0)
            return status +0x50;
    }
    SPI_write(0x0020, x0020val);

    LoadDC_REG_TX_IQ();

    //CGEN
   // SetDefaults(SECTION_CGEN);
    status = SetupCGEN();
    if(status != 0)
        return status +0x30;
	SetRxGFIR3Coefficients();
    SetNCOFrequency(LMS7002M_Tx, 9e6);
    SetNCOFrequency(LMS7002M_Rx, bandwidthRF/calibUserBwDivider - offsetNCO);
    //modifications when calibrating channel B
    if( (x0020val&0x3) == 2)
    {
        Modify_SPI_Reg_bits(MAC, 1);
        Modify_SPI_Reg_bits(EN_NEXTRX_RFE, 1);
        Modify_SPI_Reg_bits(EN_NEXTTX_TRF, 1);
        Modify_SPI_Reg_bits(PD_TX_AFE2, 0);
        SPI_write(0x0020, x0020val);
    }
    return 0;
}

uint8_t CheckSaturationRx(const float_type bandwidth_Hz)
{
    ROM const uint16_t target_rssi = 0x0B000; //0x0B000 = -3 dBFS
    uint16_t rssi;
    const uint8_t rxloopbStep = 2;
    const uint8_t cg_iampStep = 2;
    uint8_t g_rxloopb_rfe = Get_SPI_Reg_bits(G_RXLOOPB_RFE);
    uint8_t cg_iamp = Get_SPI_Reg_bits(CG_IAMP_TBB);
#ifdef DRAW_GNU_PLOTS
    std::vector<uint32_t> firstStage, secondStage;
    #define PUSH_PLOT_VALUE(vec, data) vec.push_back(data)
#else
    #define PUSH_PLOT_VALUE(vec, data)
#endif
    Modify_SPI_Reg_bits(CMIX_SC_RXTSP, 0);
    Modify_SPI_Reg_bits(CMIX_BYP_RXTSP, 0);
    SetNCOFrequency(LMS7002M_Rx, bandwidth_Hz / calibUserBwDivider - offsetNCO);

    rssi = GetRSSI();
    PUSH_PLOT_VALUE(firstStage, rssi);

#if VERBOSE
    printf("Initial gains:\tG_RXLOOPB: %2i, CG_IAMP: %2i | %2.3f dbFS\n", g_rxloopb_rfe, cg_iamp, ChipRSSI_2_dBFS(rssi));
#endif

    while (rssi < target_rssi)
    {
        g_rxloopb_rfe += rxloopbStep;
        if(g_rxloopb_rfe > 15)
        {
            g_rxloopb_rfe -= rxloopbStep;
            break;
        }
        Modify_SPI_Reg_bits(G_RXLOOPB_RFE, g_rxloopb_rfe);
        rssi = GetRSSI();
        PUSH_PLOT_VALUE(firstStage, rssi);
    }

    PUSH_PLOT_VALUE(secondStage, rssi);
    while (rssi < target_rssi)
    {
        break;
        cg_iamp += 2;
        if(cg_iamp > 20)
        {
            cg_iamp -= cg_iampStep;
            break;
        }
        Modify_SPI_Reg_bits(CG_IAMP_TBB, cg_iamp);
        rssi = GetRSSI();
        PUSH_PLOT_VALUE(secondStage, rssi);
    }
#if VERBOSE
    printf("Adjusted gains: G_RXLOOPB: %2i, CG_IAMP: %2i | %2.3f dbFS\n", g_rxloopb_rfe, cg_iamp, ChipRSSI_2_dBFS(rssi));
#endif
#ifdef DRAW_GNU_PLOTS
    saturationPlot.write("set yrange [:0]\n");
    saturationPlot.write("set ylabel 'RSSI dbFS'\n");
    saturationPlot.write("set ylabel 'measurement index'\n");
    saturationPlot.write("set title 'Rx saturation check'\n");
    saturationPlot.write("set key right bottom\n");
    saturationPlot.write("set grid ytics xtics\n");
    saturationPlot.write("plot ");

    int index = 0;
    saturationPlot.writef(
"'-' title '%s' with lines\
, '-' title 'CG_IAMP' with lines\
, '-' title 'target Level' with lines\n", "G_RXLOOPB_RFE");
    for(auto value: firstStage)
        saturationPlot.writef("%i %f\n", index++, ChipRSSI_2_dBFS(value));
    saturationPlot.write("e\n");
    --index;
    for(auto value: secondStage)
        saturationPlot.writef("%i %f\n", index++, ChipRSSI_2_dBFS(value));
    saturationPlot.write("e\n");
    saturationPlot.writef("%i %f\n%i %f\ne\n", 0, ChipRSSI_2_dBFS(target_rssi), index, ChipRSSI_2_dBFS(target_rssi));
    saturationPlot.flush();
#endif
    #undef PUSH_PLOT_VALUE
    return 0;
}

//host has to backup chip state prior calibration, and restore afterward
uint8_t CalibrateRx()
{
#ifdef __cplusplus
    auto beginTime = std::chrono::high_resolution_clock::now();
#endif
    uint8_t status;
    //uint16_t gcorri;
    //uint16_t gcorrq;
    //int16_t phaseOffset;
    const uint16_t x0020val = SPI_read(0x0020); //remember used channel

#if VERBOSE
    double rxFreq = GetFrequencySX(LMS7002M_Rx);
    const char* lnaName;
    switch(Get_SPI_Reg_bits(SEL_PATH_RFE))
    {
        case 0: lnaName = "none";
        case 1: lnaName = "LNAH";
        case 2: lnaName = "LNAW";
        case 3: lnaName = "LNAL";
    }
    printf("Rx ch.%s @ %4g MHz, BW: %g MHz, RF input: %s, PGA: %i, LNA: %i, TIA: %i\n",
                (x0020val & 0x3) == 1 ? "A" : "B", rxFreq/1e6,
                bandwidthRF/1e6, lnaName,
                Get_SPI_Reg_bits(G_PGA_RBB),
                Get_SPI_Reg_bits(G_LNA_RFE),
                Get_SPI_Reg_bits(G_TIA_RFE));
    printf("Rx calibration started\n");
#endif
	BackupRegisters();
    status = CalibrateRxSetup();
    if(status != 0)
        goto RxCalibrationEndStage;
	//return 1;
    CalibrateRxDC();
	//return 1;
    {
        if ((uint8_t)Get_SPI_Reg_bits(SEL_PATH_RFE) == 2)
        {
            Modify_SPI_Reg_bits(PD_RLOOPB_2_RFE, 0);
            Modify_SPI_Reg_bits(EN_INSHSW_LB2_RFE, 0);
        }
        else
        {
            Modify_SPI_Reg_bits(PD_RLOOPB_1_RFE, 0);
            Modify_SPI_Reg_bits(EN_INSHSW_LB1_RFE, 0);
        }
    }

    Modify_SPI_Reg_bits(MAC, 2);
    if (Get_SPI_Reg_bits(PD_LOCH_T2RBUF) == false)
    {
        Modify_SPI_Reg_bits(PD_LOCH_T2RBUF, 1);
        //TDD MODE
        Modify_SPI_Reg_bits(MAC, 1);
        Modify_SPI_Reg_bits(PD_VCO, 0);
    }
    SPI_write(0x0020, x0020val);
    CheckSaturationRx(bandwidthRF);
    Modify_SPI_Reg_bits(CMIX_SC_RXTSP, 1);
    Modify_SPI_Reg_bits(CMIX_BYP_RXTSP, 0);
    SetNCOFrequency(LMS7002M_Rx, bandwidthRF/calibUserBwDivider + offsetNCO);
    CalibrateIQImbalance(LMS7002M_Rx);
RxCalibrationEndStage:
    if (status != 0)
    {
		RestoreRegisters();
        //printf("Rx calibration failed", LOG_WARNING);
        return status;
    }
    //SPI_write(0x0020, x0020val);
	RestoreRegisters();
    //SPI_write(0x010E, toDCOffset(dcoffi) << 7 | toDCOffset((dcoffq)));
    //Modify_SPI_Reg_bits(EN_DCOFF_RXFE_RFE, 1);
    //Modify_SPI_Reg_bits(0x040C, MSBLSB(2, 0), 0); //DC_BYP 0, GC_BYP 0, PH_BYP 0
    //Modify_SPI_Reg_bits(0x0110, MSBLSB(4, 0), 31); //ICT_LO_RFE 31
    //Log("Rx calibration finished", LOG_INFO);
#if VERBOSE
    printf("#####Rx calibration RESULTS:###########################\n");
    printf("Method: %s %s loopback\n",
        "RSSI",
        "INTERNAL");
    printf("Rx ch.%s @ %4g MHz, BW: %g MHz, RF input: %s, PGA: %i, LNA: %i, TIA: %i\n",
                (x0020val & 3) == 1 ? "A" : "B", rxFreq/1e6,
                bandwidthRF/1e6, lnaName,
                Get_SPI_Reg_bits(G_PGA_RBB),
                Get_SPI_Reg_bits(G_LNA_RFE),
                Get_SPI_Reg_bits(G_TIA_RFE));
    {
        /*int8_t dcIsigned = (dcoffi & 0x3f) * (dcoffi&0x40 ? -1 : 1);
        int8_t dcQsigned = (dcoffq & 0x3f) * (dcoffq&0x40 ? -1 : 1);
        int16_t phaseSigned = phaseOffset << 4;
        phaseSigned >>= 4;
        verbose_printf("   | DC  | GAIN | PHASE\n");
        verbose_printf("---+-----+------+------\n");
        verbose_printf("I: | %3i | %4i | %i\n", dcIsigned, gcorri, phaseSigned);
        verbose_printf("Q: | %3i | %4i |\n", dcQsigned, gcorrq);*/
    }
    int32_t duration = std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::high_resolution_clock::now()-beginTime).count();
    printf("Duration: %i ms\n", duration);
#endif //LMS_VERBOSE_OUTPUT
    return 0;
}