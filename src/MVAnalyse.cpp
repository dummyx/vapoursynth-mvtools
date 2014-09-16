#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include <VapourSynth.h>
#include <VSHelper.h>

#include "dctfftw.h"
#include "MVInterface.h"
#include "GroupOfPlanes.h"


// FIXME: Redundant members. A few can go straight in analysisData.
typedef struct {
    VSNodeRef *node;
    VSVideoInfo vi;

    MVAnalysisData analysisData;
    MVAnalysisData analysisDataDivided;

    /*! \brief isse optimisations enabled */
    bool isse;

    /*! \brief motion vecteur cost factor */
    int nLambda;

    /*! \brief search type chosen for refinement in the EPZ */
    SearchType searchType;

    /*! \brief additionnal parameter for this search */
    int nSearchParam; // usually search radius

    int nPelSearch; // search radius at finest level

    int lsad; // SAD limit for lambda using - added by Fizick
    int pnew; // penalty to cost for new canditate - added by Fizick
    int plen; // penalty factor (similar to lambda) for vector length - added by Fizick
    int plevel; // penalty factors (lambda, plen) level scaling - added by Fizick
    bool global; // use global motion predictor
    int pglobal; // penalty factor for global motion predictor
    int pzero; // penalty factor for zero vector
    int divideExtra; // divide blocks on sublocks with median motion
    int badSAD; //  SAD threshold to make more wide search for bad vectors
    int badrange;// range (radius) of wide search
    bool meander; //meander (alternate) scan blocks (even row left to right, odd row right to left
    bool tryMany; // try refine around many predictors

    int dctmode;

    int nModeYUV;

    int headerSize;

    int nSuperLevels;
    int nSuperHPad;
    int nSuperVPad;
    int nSuperPel;
    int nSuperModeYUV;

    int blksize;
    int blksizev;
    int levels;
    int search;
    int searchparam;
    int isb;
    int chroma;
    int delta;
    int truemotion;
    int overlap;
    int overlapv;
    int sadx264;

    int fields;
    int tff;
    int tffexists;
} MVAnalyseData;


static void VS_CC mvanalyseInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    MVAnalyseData *d = (MVAnalyseData *) * instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}


static const VSFrameRef *VS_CC mvanalyseGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MVAnalyseData *d = (MVAnalyseData *) * instanceData;

    if (activationReason == arInitial) {
        int minframe, maxframe, nref;

        if (d->analysisData.nDeltaFrame > 0) {
            // FIXME: make it less ugly
            minframe = ( d->analysisData.isBackward ) ? 0 : d->analysisData.nDeltaFrame;
            maxframe = ( d->analysisData.isBackward ) ? (d->vi.numFrames ? d->vi.numFrames - d->analysisData.nDeltaFrame : n + d->analysisData.nDeltaFrame + 1) : d->vi.numFrames;
            int offset = ( d->analysisData.isBackward ) ? d->analysisData.nDeltaFrame : -d->analysisData.nDeltaFrame;
            nref = n + offset;
        } else { // special static mode
            nref = - d->analysisData.nDeltaFrame; // positive fixed frame number
            minframe = 0;
            maxframe = d->vi.numFrames;
        }

        if (( n < maxframe ) && ( n >= minframe )) {
            if (nref < n)
                vsapi->requestFrameFilter(nref, d->node, frameCtx);

            vsapi->requestFrameFilter(n, d->node, frameCtx);

            if (nref >= n)
                vsapi->requestFrameFilter(nref, d->node, frameCtx);
        } else {
            vsapi->requestFrameFilter(n, d->node, frameCtx);
        }
    } else if (activationReason == arAllFramesReady) {

        GroupOfPlanes *vectorFields = new GroupOfPlanes(d->analysisData.nBlkSizeX, d->analysisData.nBlkSizeY, d->analysisData.nLvCount, d->analysisData.nPel, d->analysisData.nFlags, d->analysisData.nOverlapX, d->analysisData.nOverlapY, d->analysisData.nBlkX, d->analysisData.nBlkY, d->analysisData.yRatioUV, d->divideExtra);


        const unsigned char *pSrcY, *pSrcU, *pSrcV;
        const unsigned char *pRefY, *pRefU, *pRefV;
        unsigned char *pDst;
        int nSrcPitchY, nSrcPitchUV;
        int nRefPitchY, nRefPitchUV;

        int minframe, maxframe, nref;

        if (d->analysisData.nDeltaFrame > 0) {
            minframe = ( d->analysisData.isBackward ) ? 0 : d->analysisData.nDeltaFrame;
            maxframe = ( d->analysisData.isBackward ) ? (d->vi.numFrames ? d->vi.numFrames - d->analysisData.nDeltaFrame : n + d->analysisData.nDeltaFrame + 1) : d->vi.numFrames;
            int offset = ( d->analysisData.isBackward ) ? d->analysisData.nDeltaFrame : -d->analysisData.nDeltaFrame;
            nref = n + offset;
        } else { // special static mode
            nref = - d->analysisData.nDeltaFrame; // positive fixed frame number
            minframe = 0;
            maxframe = d->vi.numFrames;
        }

        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        const VSMap *srcprops = vsapi->getFramePropsRO(src);
        int err;

        bool srctff = vsapi->propGetInt(srcprops, "_Field", 0, &err);
        if (err && d->fields && !d->tffexists) {
            vsapi->setFilterError("Analyse: _Field property not found in input frame. Therefore, you must pass tff argument.", frameCtx);
            delete vectorFields;
            vsapi->freeFrame(src);
            return NULL;
        }

        // if tff was passed, it overrides _Field.
        if (d->tffexists)
            srctff = d->tff && (n % 2 == 0); //child->GetParity(n); // bool tff;

        pSrcY = vsapi->getReadPtr(src, 0);
        pSrcU = vsapi->getReadPtr(src, 1);
        pSrcV = vsapi->getReadPtr(src, 2);
        nSrcPitchY = vsapi->getStride(src, 0);
        nSrcPitchUV = vsapi->getStride(src, 1);

        int dst_height = 1;
        int dst_width = d->headerSize / sizeof(int) + vectorFields->GetArraySize(); //v1.8.1
        // In Avisynth the frame was packed BGR32, which has 4 bytes per pixel.
        // It's GRAY8 here.
        dst_width *= 4;
        VSFrameRef *dst = vsapi->newVideoFrame(d->vi.format, dst_width, dst_height, src, core);

        pDst = vsapi->getWritePtr(dst, 0);

        // write analysis parameters as a header to frame
        memcpy(pDst, &d->headerSize, sizeof(int));

        if (d->divideExtra)
            memcpy(pDst+sizeof(int), &d->analysisDataDivided, sizeof(d->analysisData));
        else
            memcpy(pDst+sizeof(int), &d->analysisData, sizeof(d->analysisData));

        pDst += d->headerSize;


        if (( n < maxframe ) && ( n >= minframe ))
        {
            const VSFrameRef *ref = vsapi->getFrameFilter(nref, d->node, frameCtx);
            const VSMap *refprops = vsapi->getFramePropsRO(ref);

            bool reftff = vsapi->propGetInt(refprops, "_Field", 0, &err);
            if (err && d->fields && !d->tffexists) {
                vsapi->setFilterError("Analyse: _Field property not found in input frame. Therefore, you must pass tff argument.", frameCtx);
                delete vectorFields;
                vsapi->freeFrame(src);
                vsapi->freeFrame(ref);
                vsapi->freeFrame(dst);
                return NULL;
            }

            // if tff was passed, it overrides _Field.
            if (d->tffexists)
                reftff = d->tff && (nref % 2 == 0); //child->GetParity(n); // bool tff;

            int fieldShift = 0;
            if (d->fields && d->analysisData.nPel > 1 && (d->analysisData.nDeltaFrame % 2))
            {
                fieldShift = (srctff && !reftff) ? d->analysisData.nPel/2 : ( (reftff && !srctff) ? -(d->analysisData.nPel/2) : 0);
                // vertical shift of fields for fieldbased video at finest level pel2
            }

            pRefY = vsapi->getReadPtr(ref, 0);
            pRefU = vsapi->getReadPtr(ref, 1);
            pRefV = vsapi->getReadPtr(ref, 2);
            nRefPitchY = vsapi->getStride(ref, 0);
            nRefPitchUV = vsapi->getStride(ref, 1);


            MVGroupOfFrames *pSrcGOF = new MVGroupOfFrames(d->nSuperLevels, d->analysisData.nWidth, d->analysisData.nHeight, d->nSuperPel, d->nSuperHPad, d->nSuperVPad, d->nSuperModeYUV, d->isse, d->analysisData.yRatioUV);
            MVGroupOfFrames *pRefGOF = new MVGroupOfFrames(d->nSuperLevels, d->analysisData.nWidth, d->analysisData.nHeight, d->nSuperPel, d->nSuperHPad, d->nSuperVPad, d->nSuperModeYUV, d->isse, d->analysisData.yRatioUV);

            // cast away the const, because why not.
            pSrcGOF->Update(d->nModeYUV, (uint8_t *)pSrcY, nSrcPitchY, (uint8_t *)pSrcU, nSrcPitchUV, (uint8_t *)pSrcV, nSrcPitchUV); // v2.0
            pRefGOF->Update(d->nModeYUV, (uint8_t *)pRefY, nRefPitchY, (uint8_t *)pRefU, nRefPitchUV, (uint8_t *)pRefV, nRefPitchUV); // v2.0


            DCTClass *DCTc = NULL;
            if (d->dctmode != 0) {
                /*
                // FIXME: deal with this inline asm shit
                if (d->isse && (d->blksize == 8) && d->blksizev == 8)
                    DCTc = new DCTINT(d->blksize, d->blksizev, d->dctmode);
                else
                */
                DCTc = new DCTFFTW(d->blksize, d->blksizev, d->dctmode); // check order x,y
            }


            vectorFields->SearchMVs(pSrcGOF, pRefGOF, d->searchType, d->nSearchParam, d->nPelSearch, d->nLambda, d->lsad, d->pnew, d->plevel, d->global, d->analysisData.nFlags, reinterpret_cast<int*>(pDst), NULL, fieldShift, DCTc, d->pzero, d->pglobal, d->badSAD, d->badrange, d->meander, NULL, d->tryMany);

            if (d->divideExtra) {
                // make extra level with divided sublocks with median (not estimated) motion
                vectorFields->ExtraDivide(reinterpret_cast<int*>(pDst), d->analysisData.nFlags);
            }

            delete vectorFields;
            if (DCTc)
                delete DCTc;
            delete pSrcGOF;
            delete pRefGOF;
            vsapi->freeFrame(ref);
        }
        else // too close to the beginning or end to do anything
        {
            vectorFields->WriteDefaultToArray(reinterpret_cast<int*>(pDst));
            delete vectorFields;
        }


        vsapi->freeFrame(src);

        return dst;
    }

    return 0;
}


static void VS_CC mvanalyseFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    MVAnalyseData *d = (MVAnalyseData *)instanceData;

    vsapi->freeNode(d->node);
    free(d);
}


static void VS_CC mvanalyseCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    MVAnalyseData d;
    MVAnalyseData *data;

    int err;

    d.blksize = vsapi->propGetInt(in, "blksize", 0, &err);
    if (err)
        d.blksize = 8;

    d.blksizev = vsapi->propGetInt(in, "blksizev", 0, &err);
    if (err)
        d.blksizev = d.blksize;

    d.levels = vsapi->propGetInt(in, "levels", 0, &err);

    d.search = vsapi->propGetInt(in, "search", 0, &err);
    if (err)
        d.search = 4;

    d.searchparam = vsapi->propGetInt(in, "searchparam", 0, &err);
    if (err)
        d.searchparam = 2;

    d.nPelSearch = vsapi->propGetInt(in, "pelsearch", 0, &err);

    d.isb = vsapi->propGetInt(in, "isb", 0, &err);

    d.chroma = vsapi->propGetInt(in, "chroma", 0, &err);
    if (err)
        d.chroma = 1;

    d.delta = vsapi->propGetInt(in, "delta", 0, &err);
    if (err)
        d.delta = 1;

    d.truemotion = vsapi->propGetInt(in, "truemotion", 0, &err);
    if (err)
        d.truemotion = 1;

    d.nLambda = vsapi->propGetInt(in, "lambda", 0, &err);
    if (err)
        d.nLambda = d.truemotion ? (1000 * d.blksize * d.blksizev / 64) : 0;

    d.lsad = vsapi->propGetInt(in, "lsad", 0, &err);
    if (err)
        d.lsad = d.truemotion ? 1200 : 400;

    d.plevel = vsapi->propGetInt(in, "plevel", 0, &err);
    if (err)
        d.plevel = d.truemotion ? 1 : 0;

    d.global = vsapi->propGetInt(in, "global", 0, &err);
    if (err)
        d.global = d.truemotion ? 1 : 0;

    d.pnew = vsapi->propGetInt(in, "pnew", 0, &err);
    if (err)
        d.pnew = d.truemotion ? 50 : 0; // relative to 256

    d.pzero = vsapi->propGetInt(in, "pzero", 0, &err);
    if (err)
        d.pzero = d.pnew;

    d.pglobal = vsapi->propGetInt(in, "pglobal", 0, &err);

    d.overlap = vsapi->propGetInt(in, "overlap", 0, &err);

    d.overlapv = vsapi->propGetInt(in, "overlapv", 0, &err);
    if (err)
        d.overlapv = d.overlap;

    d.dctmode = vsapi->propGetInt(in, "dct", 0, &err);

    d.divideExtra = vsapi->propGetInt(in, "divide", 0, &err);

    d.sadx264 = vsapi->propGetInt(in, "sadx264", 0, &err);

    d.badSAD = vsapi->propGetInt(in, "badsad", 0, &err);
    if (err)
        d.badSAD = 10000;

    d.badrange = vsapi->propGetInt(in, "badrange", 0, &err);
    if (err)
        d.badrange = 24;

    d.isse = vsapi->propGetInt(in, "isse", 0, &err);
    if (err)
        d.isse = 0; // FIXME: used to be 1

    d.meander = vsapi->propGetInt(in, "meander", 0, &err);
    if (err)
        d.meander = 1;

    d.tryMany = vsapi->propGetInt(in, "trymany", 0, &err);

    d.fields = !!vsapi->propGetInt(in, "fields", 0, &err);

    d.tff = vsapi->propGetInt(in, "tff", 0, &err);
    d.tffexists = err;


    d.analysisData.yRatioUV = 2; //(vi.IsYV12()) ? 2 : 1;
    d.analysisData.xRatioUV = 2; // for YV12 and YUY2, really do not used and assumed to 2


    d.analysisData.nBlkSizeX = d.blksize;
    d.analysisData.nBlkSizeY = d.blksizev;
    if ((d.analysisData.nBlkSizeX != 4  || d.analysisData.nBlkSizeY != 4) &&
        (d.analysisData.nBlkSizeX != 8  || d.analysisData.nBlkSizeY != 4) &&
        (d.analysisData.nBlkSizeX != 8  || d.analysisData.nBlkSizeY != 8) &&
        (d.analysisData.nBlkSizeX != 16 || d.analysisData.nBlkSizeY != 2) &&
        (d.analysisData.nBlkSizeX != 16 || d.analysisData.nBlkSizeY != 8) &&
        (d.analysisData.nBlkSizeX != 16 || d.analysisData.nBlkSizeY != 16) &&
        (d.analysisData.nBlkSizeX != 32 || d.analysisData.nBlkSizeY != 32) &&
        (d.analysisData.nBlkSizeX != 32 || d.analysisData.nBlkSizeY != 16)) {

        vsapi->setError(out, "Analyse: the block size must be 4x4, 8x4, 8x8, 16x2, 16x8, 16x16, 32x16, or 32x32.");
        return;
    }


    d.analysisData.nDeltaFrame = d.delta;


    if (d.overlap < 0 || d.overlap >= d.blksize ||
        d.overlapv < 0 || d.overlapv >= d.blksizev) {
        vsapi->setError(out, "Analyse: overlap must be less than blksize, and overlapv must be less than blksizev.");
        return;
    }

    if (d.overlap % 2 || d.overlapv % 2) { // subsampling
        vsapi->setError(out, "Analyse: overlap and overlapv must be multiples of 2.");
        return;
    }

    if (d.divideExtra && (d.blksize < 8 && d.blksizev < 8) ) {
        vsapi->setError(out, "Analyse: blksize and blksizev must be at least 8 when divide=True.");
        return;
    }

    if (d.divideExtra && (d.overlap % 4 || d.overlapv % 4)) { // subsampling times 2
        vsapi->setError(out, "Analyse: overlap and overlapv must be multiples of 4 when divide=True.");
        return;
    }

    d.analysisData.nOverlapX = d.overlap;
    d.analysisData.nOverlapY = d.overlapv;

    d.analysisData.isBackward = d.isb;


    d.lsad = d.lsad * (d.blksize * d.blksizev) / 64;
    d.badSAD = d.badSAD * (d.blksize * d.blksizev) / 64;

    SearchType searchTypes[] = { ONETIME, NSTEP, LOGARITHMIC, EXHAUSTIVE, HEX2SEARCH, UMHSEARCH, HSEARCH, VSEARCH };
    d.searchType = searchTypes[d.search];

    if (d.searchType == NSTEP)
        d.nSearchParam = ( d.searchparam < 0 ) ? 0 : d.searchparam;
    else
        d.nSearchParam = ( d.searchparam < 1 ) ? 1 : d.searchparam;


    d.analysisData.nFlags = 0;
    d.analysisData.nFlags |= d.isse ? MOTION_USE_ISSE : 0;
    d.analysisData.nFlags |= d.analysisData.isBackward ? MOTION_IS_BACKWARD : 0;
    d.analysisData.nFlags |= d.chroma ? MOTION_USE_CHROMA_MOTION : 0;


    if (d.sadx264 == 0)
    {
        // FIXME: get cpu_detect() working
        d.analysisData.nFlags |= 0; //cpu_detect();
    }
    else
    {
        if ((d.sadx264 > 0) && (d.sadx264 <= 12))
        {
            //force specific function
            d.analysisData.nFlags |= CPU_MMXEXT;
            d.analysisData.nFlags |= (d.sadx264 == 2) ? CPU_CACHELINE_32 : 0;
            d.analysisData.nFlags |= ((d.sadx264 == 3) || (d.sadx264 == 5) || (d.sadx264 == 7)) ? CPU_CACHELINE_64 : 0;
            d.analysisData.nFlags |= ((d.sadx264 == 4) || (d.sadx264 == 5) || (d.sadx264 == 10)) ? CPU_SSE2_IS_FAST : 0;
            d.analysisData.nFlags |= (d.sadx264 == 6) ? CPU_SSE3 : 0;
            d.analysisData.nFlags |= ((d.sadx264 == 7) || (d.sadx264 >= 11)) ? CPU_SSSE3 : 0;
            //beta (debug)
            d.analysisData.nFlags |= (d.sadx264 == 8) ? MOTION_USE_SSD : 0;
            d.analysisData.nFlags |= ((d.sadx264 >= 9) && (d.sadx264 <= 12)) ? MOTION_USE_SATD : 0;
            d.analysisData.nFlags |= (d.sadx264 == 12) ? CPU_PHADD_IS_FAST : 0;
        }
    }

    d.nModeYUV = d.chroma ? YUVPLANES : YPLANE;

    // XXX maybe get rid of these two
    // Bleh, they're checked by client filters. Though it's kind of pointless.
    d.analysisData.nMagicKey = MOTION_MAGIC_KEY;
    d.analysisData.nVersion = MVANALYSIS_DATA_VERSION; // MVAnalysisData and outfile format version: last update v1.8.1


    d.headerSize = VSMAX(4 + sizeof(d.analysisData), 256); // include itself, but usually equal to 256 :-)


    d.node = vsapi->propGetNode(in, "super", 0, 0);
    d.vi = *vsapi->getVideoInfo(d.node);

    if (!isConstantFormat(&d.vi) || d.vi.format->id != pfYUV420P8) {
        vsapi->setError(out, "Analyse: Input clip must be YUV420P8 with constant format and dimensions.");
        vsapi->freeNode(d.node);
        return;
    }


    char errorMsg[1024];
    const VSFrameRef *evil = vsapi->getFrame(0, d.node, errorMsg, 1024);
    if (!evil) {
        vsapi->setError(out, std::string("Analyse: failed to retrieve first frame from super clip. Error message: ").append(errorMsg).c_str());
        vsapi->freeNode(d.node);
        return;
    }
    const VSMap *props = vsapi->getFramePropsRO(evil);
    int evil_err[6];
    int nHeight = vsapi->propGetInt(props, "Super height", 0, &evil_err[0]);
    d.nSuperHPad = vsapi->propGetInt(props, "Super hpad", 0, &evil_err[1]);
    d.nSuperVPad = vsapi->propGetInt(props, "Super vpad", 0, &evil_err[2]);
    d.nSuperPel = vsapi->propGetInt(props, "Super pel", 0, &evil_err[3]);
    d.nSuperModeYUV = vsapi->propGetInt(props, "Super modeyuv", 0, &evil_err[4]);
    d.nSuperLevels = vsapi->propGetInt(props, "Super levels", 0, &evil_err[5]);
    vsapi->freeFrame(evil);

    for (int i = 0; i < 6; i++)
        if (evil_err[i]) {
            vsapi->setError(out, "Analyse: required properties not found in first frame of super clip. Maybe clip didn't come from mv.Super? Was the first frame trimmed away?");
            vsapi->freeNode(d.node);
            return;
        }

    // check sanity
    if (nHeight <= 0 || d.nSuperHPad < 0 || d.nSuperHPad >= d.vi.width / 2 ||
        d.nSuperVPad < 0 || d.nSuperPel < 1 || d.nSuperPel > 4 ||
        d.nSuperModeYUV < 0 || d.nSuperModeYUV > YUVPLANES || d.nSuperLevels < 1) {
        vsapi->setError(out, "Analyse: parameters from super clip appear to be wrong.");
        vsapi->freeNode(d.node);
        return;
    }

    if ((d.nModeYUV & d.nSuperModeYUV) != d.nModeYUV) { //x
        vsapi->setError(out, "Analyse: super clip does not contain needed colour data.");
        vsapi->freeNode(d.node);
        return;
    }


    // fill in missing fields
    d.analysisData.nWidth = d.vi.width - d.nSuperHPad*2;//x

    d.analysisData.nHeight = nHeight;//x

    d.analysisData.nPel = d.nSuperPel;//x
    // mv.Super already checks this
    /*
       if (( d.analysisData.nPel != 1 ) && ( d.analysisData.nPel != 2 ) && ( d.analysisData.nPel != 4 ))
       env->ThrowError("MAnalyse: pel has to be 1 or 2 or 4");
       */

    d.analysisData.nHPadding = d.nSuperHPad; //v2.0    //x
    d.analysisData.nVPadding = d.nSuperVPad;


    int nBlkX = (d.analysisData.nWidth - d.analysisData.nOverlapX) / (d.analysisData.nBlkSizeX - d.analysisData.nOverlapX);//x

    int nBlkY = (d.analysisData.nHeight - d.analysisData.nOverlapY) / (d.analysisData.nBlkSizeY - d.analysisData.nOverlapY);

    d.analysisData.nBlkX = nBlkX;
    d.analysisData.nBlkY = nBlkY;

    int nWidth_B = (d.analysisData.nBlkSizeX - d.analysisData.nOverlapX) * nBlkX + d.analysisData.nOverlapX; // covered by blocks
    int nHeight_B = (d.analysisData.nBlkSizeY - d.analysisData.nOverlapY) * nBlkY + d.analysisData.nOverlapY;

    // calculate valid levels
    int nLevelsMax = 0;
    while (	((nWidth_B >> nLevelsMax) - d.analysisData.nOverlapX) / (d.analysisData.nBlkSizeX - d.analysisData.nOverlapX) > 0 &&
            ((nHeight_B >> nLevelsMax) - d.analysisData.nOverlapY) / (d.analysisData.nBlkSizeY - d.analysisData.nOverlapY) > 0) // at last one block
    {
        nLevelsMax++;
    }

    d.analysisData.nLvCount = d.levels > 0 ? d.levels : nLevelsMax + d.levels;

    if (d.analysisData.nLvCount < 1 || d.analysisData.nLvCount > nLevelsMax) {
        vsapi->setError(out, "Analyse: invalid number of levels.");
        vsapi->freeNode(d.node);
        return;
    }

    if (d.analysisData.nLvCount > d.nSuperLevels) { //x
        vsapi->setError(out, ("Analyse: super clip has " + std::to_string(d.nSuperLevels) + " levels. Analyse needs " + std::to_string(d.analysisData.nLvCount) + " levels.").c_str());
        vsapi->freeNode(d.node);
        return;
    }


    if (d.nPelSearch <= 0)
        d.nPelSearch = d.analysisData.nPel; // not below value of 0 at finest level //x


    if (d.divideExtra) { //v1.8.1
        memcpy(&d.analysisDataDivided, &d.analysisData, sizeof(d.analysisData));
        d.analysisDataDivided.nBlkX = d.analysisData.nBlkX * 2;
        d.analysisDataDivided.nBlkY = d.analysisData.nBlkY * 2;
        d.analysisDataDivided.nBlkSizeX = d.analysisData.nBlkSizeX / 2;
        d.analysisDataDivided.nBlkSizeY = d.analysisData.nBlkSizeY / 2;
        d.analysisDataDivided.nOverlapX = d.analysisData.nOverlapX / 2;
        d.analysisDataDivided.nOverlapY = d.analysisData.nOverlapY / 2;
        d.analysisDataDivided.nLvCount = d.analysisData.nLvCount + 1;
    }


    // XXX Can't know the output width yet, because vectorFields doesn't exist here. Will just return a clip with unknown dimensions.
    /*
    // vector steam packed in
    d.vi.height = 1;
    d.vi.width = d.headerSize / sizeof(int) + vectorFields->GetArraySize(); //v1.8.1
    vi.pixel_type = VideoInfo::CS_BGR32;
    vi.audio_samples_per_second = 0; //v1.8.1
    */

    d.vi.width = d.vi.height = 0;

    // Most similar to packed BGR32.
    d.vi.format = vsapi->getFormatPreset(pfGray8, core);


    data = (MVAnalyseData *)malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Analyse", mvanalyseInit, mvanalyseGetFrame, mvanalyseFree, fmParallel, 0, data, core);
}


void mvanalyseRegister(VSRegisterFunction registerFunc, VSPlugin *plugin) {
    registerFunc("Analyse",
                 "super:clip;"
                 "blksize:int:opt;"
                 "blksizev:int:opt;"
                 "levels:int:opt;"
                 "search:int:opt;"
                 "searchparam:int:opt;"
                 "pelsearch:int:opt;"
                 "isb:int:opt;"
                 "lambda:int:opt;"
                 "chroma:int:opt;"
                 "delta:int:opt;"
                 "truemotion:int:opt;"
                 "lsad:int:opt;"
                 "plevel:int:opt;"
                 "global:int:opt;"
                 "pnew:int:opt;"
                 "pzero:int:opt;"
                 "pglobal:int:opt;"
                 "overlap:int:opt;"
                 "overlapv:int:opt;"
                 "dct:int:opt;"
                 "divide:int:opt;"
                 "sadx264:int:opt;"
                 "badsad:int:opt;"
                 "badrange:int:opt;"
                 "isse:int:opt;"
                 "meander:int:opt;"
                 "trymany:int:opt;"
                 "fields:int:opt;"
                 "tff:int:opt;"
                 , mvanalyseCreate, 0, plugin);
}
