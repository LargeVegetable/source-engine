//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: NEON versions of the software flex inner loops (R_StudioFlexVerts,
//          CCachedRenderData::RenormalizeFlexVertices). Plain functions on
//          floats and shorts, so a test on any NEON host can compare them
//          with the scalar code.
//
//=============================================================================//
#ifndef FLEXNEON_H
#define FLEXNEON_H

#if defined( __ARM_NEON__ ) || defined( __ARM_NEON )
#include <arm_neon.h>
#define STUDIO_FLEX_NEON 1

// One flexed vertex: position and normal (six contiguous floats), and the
// tangent's xyz when pTangent is set, get the vertex animation's deltas
// (delta[3] then ndelta[3], fixed point) times flScaledWeight (the flex
// weight times the model's fixed-point scale). The scalar code converts and
// scales each component, then weighs it; here it's one convert and one
// multiply-add per four components. pPosNormal's six floats are read and
// written as one 16-byte and one 8-byte access
inline void FlexAccumulateNeon( float *pPosNormal, float *pTangent, const short *pDeltas, float flScaledWeight )
{
	// d0 d1 d2 n0 and d2 n0 n1 n2
	const float32x4_t vLow = vcvtq_f32_s32( vmovl_s16( vld1_s16( pDeltas ) ) );
	const float32x4_t vHigh = vcvtq_f32_s32( vmovl_s16( vld1_s16( pDeltas + 2 ) ) );

	// position xyz and normal x
	float32x4_t vPos = vld1q_f32( pPosNormal );
	vPos = vmlaq_n_f32( vPos, vLow, flScaledWeight );
	vst1q_f32( pPosNormal, vPos );

	// normal yz
	float32x2_t vNormYZ = vld1_f32( pPosNormal + 4 );
	vNormYZ = vmla_n_f32( vNormYZ, vget_high_f32( vHigh ), flScaledWeight );
	vst1_f32( pPosNormal + 4, vNormYZ );

	if ( pTangent )
	{
		// n0 n1 n2 into the tangent's xyz, its w (the sign) untouched
		float32x4_t vN = vextq_f32( vHigh, vHigh, 1 );		// n0 n1 n2 d2
		vN = vsetq_lane_f32( 0.0f, vN, 3 );
		float32x4_t vTan = vld1q_f32( pTangent );
		vTan = vmlaq_n_f32( vTan, vN, flScaledWeight );
		vst1q_f32( pTangent, vTan );
	}
}

// 1 / (sqrt( x ) + FLT_EPSILON) for two lengths squared at once, as
// _VectorNormalize computes it: reciprocal square root and reciprocal
// estimates with two Newton-Raphson steps each (a few units in the last
// place from the VFP's sqrt and divide); 0 stays 0 length, so the vector is
// zeroed like the scalar code does
inline float32x2_t FlexInvLengthNeon( float32x2_t vLen2 )
{
	float32x2_t vR = vrsqrte_f32( vLen2 );
	vR = vmul_f32( vR, vrsqrts_f32( vmul_f32( vLen2, vR ), vR ) );
	vR = vmul_f32( vR, vrsqrts_f32( vmul_f32( vLen2, vR ), vR ) );
	float32x2_t vLen = vmul_f32( vLen2, vR );
	// x = 0: the estimate is infinite and 0 * inf is NaN; the length is 0
	const uint32x2_t vZero = vceq_f32( vLen2, vdup_n_f32( 0.0f ) );
	vLen = vbsl_f32( vZero, vdup_n_f32( 0.0f ), vLen );
	const float32x2_t vDen = vadd_f32( vLen, vdup_n_f32( 1.192092896e-07F ) );
	float32x2_t vInv = vrecpe_f32( vDen );
	vInv = vmul_f32( vInv, vrecps_f32( vDen, vInv ) );
	vInv = vmul_f32( vInv, vrecps_f32( vDen, vInv ) );
	return vInv;
}

// Normalizes a normal's xyz and, when pTangent is set, a tangent's xyz
// (its w left alone). Reads 16 bytes at pNormal: the caller's struct must
// have a float after the normal
inline void FlexRenormalizeNeon( float *pNormal, float *pTangent )
{
	const float32x4_t vN = vld1q_f32( pNormal );
	const float32x4_t vT = pTangent ? vld1q_f32( pTangent ) : vN;
	float32x4_t vNN = vmulq_f32( vN, vN );
	float32x4_t vTT = vmulq_f32( vT, vT );
	// x*x + y*y in each half's lane 0, then + z*z
	float32x2_t vSums = vpadd_f32( vget_low_f32( vNN ), vget_low_f32( vTT ) );
	vSums = vadd_f32( vSums, vzip_f32( vget_high_f32( vNN ), vget_high_f32( vTT ) ).val[0] );
	const float32x2_t vInv = FlexInvLengthNeon( vSums );

	const float32x4_t vNs = vmulq_lane_f32( vN, vInv, 0 );
	vst1_f32( pNormal, vget_low_f32( vNs ) );
	vst1_lane_f32( pNormal + 2, vget_high_f32( vNs ), 0 );
	if ( pTangent )
	{
		const float32x4_t vTs = vmulq_lane_f32( vT, vInv, 1 );
		vst1_f32( pTangent, vget_low_f32( vTs ) );
		vst1_lane_f32( pTangent + 2, vget_high_f32( vTs ), 0 );
	}
}

#endif // NEON

#endif // FLEXNEON_H
