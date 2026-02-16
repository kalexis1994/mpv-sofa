/*
 * QMF (Quadrature Mirror Filter) analysis/synthesis for object coding reconstruction
 * Per ETSI TS 103 420 Section 7: n=64 subbands, n_qmf_length=640
 *
 * Analysis: 64 PCM samples → 64 complex subband values
 * Synthesis: 64 complex subband values → 64 PCM samples
 */

#ifndef OBJCODING_QMF_H
#define OBJCODING_QMF_H

#define OBJCODING_QMF_N       64   /* number of subbands */
#define OBJCODING_QMF_LEN    640   /* filter length (10 * n) */

typedef struct {
    float state[OBJCODING_QMF_LEN];  /* qana_filt shift register */
} ObjCodingQmfAnalysis;

typedef struct {
    float state[2 * OBJCODING_QMF_LEN];  /* qsyn_filt: 2 * n_qmf_length */
} ObjCodingQmfSynthesis;

/* Initialize analysis/synthesis filterbank state to zero */
void objcoding_qmf_analysis_init(ObjCodingQmfAnalysis *a);
void objcoding_qmf_synthesis_init(ObjCodingQmfSynthesis *s);

/*
 * Analyze one timeslot: 64 PCM samples → 64 complex subband values
 * out_re[64], out_im[64] are the real and imaginary parts
 */
void objcoding_qmf_analyze(ObjCodingQmfAnalysis *a, const float *pcm_64,
                      float out_re[OBJCODING_QMF_N], float out_im[OBJCODING_QMF_N]);

/*
 * Synthesize one timeslot: 64 complex subband values → 64 PCM samples
 * re[64], im[64] are real/imaginary input; pcm_out[64] is output
 */
void objcoding_qmf_synthesize(ObjCodingQmfSynthesis *s, const float re[OBJCODING_QMF_N],
                         const float im[OBJCODING_QMF_N], float pcm_out[OBJCODING_QMF_N]);

#endif /* OBJCODING_QMF_H */
