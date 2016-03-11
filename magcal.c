// Copyright (c) 2014, Freescale Semiconductor, Inc.
// All rights reserved.
// vim: set ts=4:
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Freescale Semiconductor, Inc. nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL FREESCALE SEMICONDUCTOR, INC. BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// This file contains magnetic calibration functions.  It is STRONGLY RECOMMENDED
// that the casual developer NOT TOUCH THIS FILE.  The mathematics behind this file
// is extremely complex, and it will be very easy (almost inevitable) that you screw
// it up.
//
// Haha - This file has been edited!  Please do not blame or pester NXP (formerly
//        Freescale) about the "almost inevitable" issues!

#include "imuread.h"

#define FXOS8700_UTPERCOUNT  0.1f
#define DEFAULTB 50.0F				// default geomagnetic field (uT)
#define X 0                         // vector components
#define Y 1
#define Z 2
#define ONETHIRD 0.33333333F        // one third
#define ONESIXTH 0.166666667F       // one sixth
#define MINMEASUREMENTS4CAL 40      // minimum number of measurements for 4 element calibration
#define MINMEASUREMENTS7CAL 100     // minimum number of measurements for 7 element calibration
#define MINMEASUREMENTS10CAL 150    // minimum number of measurements for 10 element calibration
#define MINBFITUT 22.0F             // minimum geomagnetic field B (uT) for valid calibration
#define MAXBFITUT 67.0F             // maximum geomagnetic field B (uT) for valid calibration
#define FITERRORAGINGSECS 7200.0F   // 2 hours: time for fit error to increase (age) by e=2.718

static void fUpdateCalibration4INV(MagCalibration_t *MagCal);
static void fUpdateCalibration7EIG(MagCalibration_t *MagCal);
static void fUpdateCalibration10EIG(MagCalibration_t *MagCal);



// run the magnetic calibration
void MagCal_Run(void)
{
	int i, j;			// loop counters
	int isolver;		// magnetic solver used
	int count=0;
	static int waitcount=0;

	// only do the calibration occasionally
	if (++waitcount < 20) return;
	waitcount = 0;

	// count number of data points
	for (i=0; i < MAGBUFFSIZE; i++) {
		if (magcal.valid[i]) count++;
	}

	if (count < MINMEASUREMENTS4CAL) return;

	if (magcal.iValidMagCal) {
		// age the existing fit error to avoid one good calibration locking out future updates
		magcal.fFitErrorpc *= 1.02f;
	}

	// is enough data collected
	if (count < MINMEASUREMENTS7CAL) {
		isolver = 4;
		fUpdateCalibration4INV(&magcal); // 4 element matrix inversion calibration
	} else if (count < MINMEASUREMENTS10CAL) {
		isolver = 7;
		fUpdateCalibration7EIG(&magcal); // 7 element eigenpair calibration
	} else {
		isolver = 10;
		fUpdateCalibration10EIG(&magcal); // 10 element eigenpair calibration
	}

	// the trial geomagnetic field must be in range (earth is 22uT to 67uT)
	if ((magcal.ftrB >= MINBFITUT) && (magcal.ftrB <= MAXBFITUT))	{
		// always accept the calibration if
		//  1: no previous calibration exists
		//  2: the calibration fit is reduced or
		//  3: an improved solver was used giving a good trial calibration (4% or under)
		if ((magcal.iValidMagCal == 0) ||
				(magcal.ftrFitErrorpc <= magcal.fFitErrorpc) ||
				((isolver > magcal.iValidMagCal) && (magcal.ftrFitErrorpc <= 4.0F))) {
			// accept the new calibration solution
			//printf("new magnetic cal, B=%.2f uT\n", magcal.ftrB);
			magcal.iValidMagCal = isolver;
			magcal.fFitErrorpc = magcal.ftrFitErrorpc;
			magcal.fB = magcal.ftrB;
			magcal.fFourBsq = 4.0F * magcal.ftrB * magcal.ftrB;
			for (i = X; i <= Z; i++) {
				magcal.fV[i] = magcal.ftrV[i];
				for (j = X; j <= Z; j++) {
					magcal.finvW[i][j] = magcal.ftrinvW[i][j];
				}
			}
		}
	}
}



// 4 element calibration using 4x4 matrix inverse
static void fUpdateCalibration4INV(MagCalibration_t *MagCal)
{
	float fBp2;					// fBp[X]^2+fBp[Y]^2+fBp[Z]^2
	float fSumBp4;				// sum of fBp2
	float fscaling;				// set to FUTPERCOUNT * FMATRIXSCALING
	float fE;					// error function = r^T.r
	int16_t iOffset[3];			// offset to remove large DC hard iron bias in matrix
	int16_t iCount;				// number of measurements counted
	int i, j, k;				// loop counters
	
	// working arrays for 4x4 matrix inversion
	float *pfRows[4];
	int8_t iColInd[4];
	int8_t iRowInd[4];
	int8_t iPivot[4];

	// compute fscaling to reduce multiplications later
	fscaling = FXOS8700_UTPERCOUNT / DEFAULTB;

	// the trial inverse soft iron matrix invW always equals
	// the identity matrix for 4 element calibration
	f3x3matrixAeqI(MagCal->ftrinvW);

	// zero fSumBp4=Y^T.Y, fvecB=X^T.Y (4x1) and on and above
	// diagonal elements of fmatA=X^T*X (4x4)
	fSumBp4 = 0.0F;
	for (i = 0; i < 4; i++) {
		MagCal->fvecB[i] = 0.0F;
		for (j = i; j < 4; j++) {
			MagCal->fmatA[i][j] = 0.0F;
		}
	}

	// the offsets are guaranteed to be set from the first element but to avoid compiler error
	iOffset[X] = iOffset[Y] = iOffset[Z] = 0;

	// use from MINEQUATIONS up to MAXEQUATIONS entries from magnetic buffer to compute matrices
	iCount = 0;
	for (j = 0; j < MAGBUFFSIZE; j++) {
		if (MagCal->valid[j]) {
			// use first valid magnetic buffer entry as estimate (in counts) for offset
			if (iCount == 0) {
				for (k = X; k <= Z; k++) {
					iOffset[k] = MagCal->iBpFast[k][j];
				}
			}

			// store scaled and offset fBp[XYZ] in fvecA[0-2] and fBp[XYZ]^2 in fvecA[3-5]
			for (k = X; k <= Z; k++) {
				MagCal->fvecA[k] = (float)((int32_t)MagCal->iBpFast[k][j]
					- (int32_t)iOffset[k]) * fscaling;
				MagCal->fvecA[k + 3] = MagCal->fvecA[k] * MagCal->fvecA[k];
			}

			// calculate fBp2 = fBp[X]^2 + fBp[Y]^2 + fBp[Z]^2 (scaled uT^2)
			fBp2 = MagCal->fvecA[3] + MagCal->fvecA[4] + MagCal->fvecA[5];

			// accumulate fBp^4 over all measurements into fSumBp4=Y^T.Y
			fSumBp4 += fBp2 * fBp2;

			// now we have fBp2, accumulate fvecB[0-2] = X^T.Y =sum(fBp2.fBp[XYZ])
			for (k = X; k <= Z; k++) {
				MagCal->fvecB[k] += MagCal->fvecA[k] * fBp2;
			}

			//accumulate fvecB[3] = X^T.Y =sum(fBp2)
			MagCal->fvecB[3] += fBp2;

			// accumulate on and above-diagonal terms of fmatA = X^T.X ignoring fmatA[3][3]
			MagCal->fmatA[0][0] += MagCal->fvecA[X + 3];
			MagCal->fmatA[0][1] += MagCal->fvecA[X] * MagCal->fvecA[Y];
			MagCal->fmatA[0][2] += MagCal->fvecA[X] * MagCal->fvecA[Z];
			MagCal->fmatA[0][3] += MagCal->fvecA[X];
			MagCal->fmatA[1][1] += MagCal->fvecA[Y + 3];
			MagCal->fmatA[1][2] += MagCal->fvecA[Y] * MagCal->fvecA[Z];
			MagCal->fmatA[1][3] += MagCal->fvecA[Y];
			MagCal->fmatA[2][2] += MagCal->fvecA[Z + 3];
			MagCal->fmatA[2][3] += MagCal->fvecA[Z];

			// increment the counter for next iteration
			iCount++;
		}
	}

	// set the last element of the measurement matrix to the number of buffer elements used
	MagCal->fmatA[3][3] = (float) iCount;

	// store the number of measurements accumulated
	MagCal->iMagBufferCount = iCount;

	// use above diagonal elements of symmetric fmatA to set both fmatB and fmatA to X^T.X
	for (i = 0; i < 4; i++) {
		for (j = i; j < 4; j++) {
			MagCal->fmatB[i][j] = MagCal->fmatB[j][i]
				= MagCal->fmatA[j][i] = MagCal->fmatA[i][j];
		}
	}

	// calculate in situ inverse of fmatB = inv(X^T.X) (4x4) while fmatA still holds X^T.X
	for (i = 0; i < 4; i++) {
		pfRows[i] = MagCal->fmatB[i];
	}
	fmatrixAeqInvA(pfRows, iColInd, iRowInd, iPivot, 4);

	// calculate fvecA = solution beta (4x1) = inv(X^T.X).X^T.Y = fmatB * fvecB
	for (i = 0; i < 4; i++) {
		MagCal->fvecA[i] = 0.0F;
		for (k = 0; k < 4; k++) {
			MagCal->fvecA[i] += MagCal->fmatB[i][k] * MagCal->fvecB[k];
		}
	}

	// calculate P = r^T.r = Y^T.Y - 2 * beta^T.(X^T.Y) + beta^T.(X^T.X).beta
	// = fSumBp4 - 2 * fvecA^T.fvecB + fvecA^T.fmatA.fvecA
	// first set P = Y^T.Y - 2 * beta^T.(X^T.Y) = fSumBp4 - 2 * fvecA^T.fvecB
	fE = 0.0F;
	for (i = 0; i < 4; i++) {
		fE += MagCal->fvecA[i] * MagCal->fvecB[i];
	}
	fE = fSumBp4 - 2.0F * fE;

	// set fvecB = (X^T.X).beta = fmatA.fvecA	
	for (i = 0; i < 4; i++) {
		MagCal->fvecB[i] = 0.0F;
		for (k = 0; k < 4; k++) {
			MagCal->fvecB[i] += MagCal->fmatA[i][k] * MagCal->fvecA[k];
		}
	}

	// complete calculation of P by adding beta^T.(X^T.X).beta = fvecA^T * fvecB
	for (i = 0; i < 4; i++) {
		fE += MagCal->fvecB[i] * MagCal->fvecA[i];
	}

	// compute the hard iron vector (in uT but offset and scaled by FMATRIXSCALING)
	for (k = X; k <= Z; k++) {
		MagCal->ftrV[k] = 0.5F * MagCal->fvecA[k];
	}

	// compute the scaled geomagnetic field strength B (in uT but scaled by FMATRIXSCALING)
	MagCal->ftrB = sqrtf(MagCal->fvecA[3] + MagCal->ftrV[X] * MagCal->ftrV[X] +
			MagCal->ftrV[Y] * MagCal->ftrV[Y] + MagCal->ftrV[Z] * MagCal->ftrV[Z]);

	// calculate the trial fit error (percent) normalized to number of measurements
	// and scaled geomagnetic field strength
	MagCal->ftrFitErrorpc = sqrtf(fE / (float) MagCal->iMagBufferCount) * 100.0F /
			(2.0F * MagCal->ftrB * MagCal->ftrB);

	// correct the hard iron estimate for FMATRIXSCALING and the offsets applied (result in uT)
	for (k = X; k <= Z; k++) {
		MagCal->ftrV[k] = MagCal->ftrV[k] * DEFAULTB
			+ (float)iOffset[k] * FXOS8700_UTPERCOUNT;
	}

	// correct the geomagnetic field strength B to correct scaling (result in uT)
	MagCal->ftrB *= DEFAULTB;
}










// 7 element calibration using direct eigen-decomposition
static void fUpdateCalibration7EIG(MagCalibration_t *MagCal)
{
	float det;					// matrix determinant
	float fscaling;				// set to FUTPERCOUNT * FMATRIXSCALING
	float ftmp;					// scratch variable
	int16_t iOffset[3];			// offset to remove large DC hard iron bias
	int16_t iCount;				// number of measurements counted
	int i, j, k, m, n;			// loop counters

	// compute fscaling to reduce multiplications later
	fscaling = FXOS8700_UTPERCOUNT / DEFAULTB;

	// the offsets are guaranteed to be set from the first element but to avoid compiler error
	iOffset[X] = iOffset[Y] = iOffset[Z] = 0;

	// zero the on and above diagonal elements of the 7x7 symmetric measurement matrix fmatA
	for (m = 0; m < 7; m++) {
		for (n = m; n < 7; n++) {
			MagCal->fmatA[m][n] = 0.0F;
		}
	}

	// place from MINEQUATIONS to MAXEQUATIONS entries into product matrix fmatA
	iCount = 0;
	for (j = 0; j < MAGBUFFSIZE; j++) {
		if (MagCal->valid[j]) {
			// use first valid magnetic buffer entry as offset estimate (bit counts)
			if (iCount == 0) {
				for (k = X; k <= Z; k++) {
					iOffset[k] = MagCal->iBpFast[k][j];
				}
			}

			// apply the offset and scaling and store in fvecA
			for (k = X; k <= Z; k++) {
				MagCal->fvecA[k + 3] = (float)((int32_t)MagCal->iBpFast[k][j]
					- (int32_t)iOffset[k]) * fscaling;
				MagCal->fvecA[k] = MagCal->fvecA[k + 3] * MagCal->fvecA[k + 3];
			}

			// accumulate the on-and above-diagonal terms of
			// MagCal->fmatA=Sigma{fvecA^T * fvecA}
			// with the exception of fmatA[6][6] which will sum to the number
			// of measurements and remembering that fvecA[6] equals 1.0F
			// update the right hand column [6] of fmatA except for fmatA[6][6]
			for (m = 0; m < 6; m++) {
				MagCal->fmatA[m][6] += MagCal->fvecA[m];
			}
			// update the on and above diagonal terms except for right hand column 6
			for (m = 0; m < 6; m++) {
				for (n = m; n < 6; n++) {
					MagCal->fmatA[m][n] += MagCal->fvecA[m] * MagCal->fvecA[n];
				}
			}

			// increment the measurement counter for the next iteration
			iCount++;
		}
	}

	// finally set the last element fmatA[6][6] to the number of measurements
	MagCal->fmatA[6][6] = (float) iCount;

	// store the number of measurements accumulated
	MagCal->iMagBufferCount = iCount;

	// copy the above diagonal elements of fmatA to below the diagonal
	for (m = 1; m < 7; m++) {
		for (n = 0; n < m; n++) {
			MagCal->fmatA[m][n] = MagCal->fmatA[n][m];
		}
	}

	// set tmpA7x1 to the unsorted eigenvalues and fmatB to the unsorted eigenvectors of fmatA
	eigencompute(MagCal->fmatA, MagCal->fvecA, MagCal->fmatB, 7);

	// find the smallest eigenvalue
	j = 0;
	for (i = 1; i < 7; i++) {
		if (MagCal->fvecA[i] < MagCal->fvecA[j]) {
			j = i;
		}
	}

	// set ellipsoid matrix A to the solution vector with smallest eigenvalue,
	// compute its determinant and the hard iron offset (scaled and offset)
	f3x3matrixAeqScalar(MagCal->fA, 0.0F);
	det = 1.0F;
	for (k = X; k <= Z; k++) {
		MagCal->fA[k][k] = MagCal->fmatB[k][j];
		det *= MagCal->fA[k][k];
		MagCal->ftrV[k] = -0.5F * MagCal->fmatB[k + 3][j] / MagCal->fA[k][k];
	}

	// negate A if it has negative determinant
	if (det < 0.0F) {
		f3x3matrixAeqMinusA(MagCal->fA);
		MagCal->fmatB[6][j] = -MagCal->fmatB[6][j];
		det = -det;
	}

	// set ftmp to the square of the trial geomagnetic field strength B
	// (counts times FMATRIXSCALING)
	ftmp = -MagCal->fmatB[6][j];
	for (k = X; k <= Z; k++) {
		ftmp += MagCal->fA[k][k] * MagCal->ftrV[k] * MagCal->ftrV[k];
	}

	// calculate the trial normalized fit error as a percentage
	MagCal->ftrFitErrorpc = 50.0F *
		sqrtf(fabs(MagCal->fvecA[j]) / (float) MagCal->iMagBufferCount) / fabs(ftmp);

	// normalize the ellipsoid matrix A to unit determinant
	f3x3matrixAeqAxScalar(MagCal->fA, powf(det, -(ONETHIRD)));

	// convert the geomagnetic field strength B into uT for normalized
	// soft iron matrix A and normalize
	MagCal->ftrB = sqrtf(fabs(ftmp)) * DEFAULTB * powf(det, -(ONESIXTH));

	// compute trial invW from the square root of A also with normalized
	// determinant and hard iron offset in uT
	f3x3matrixAeqI(MagCal->ftrinvW);
	for (k = X; k <= Z; k++) {
		MagCal->ftrinvW[k][k] = sqrtf(fabs(MagCal->fA[k][k]));
		MagCal->ftrV[k] = MagCal->ftrV[k] * DEFAULTB + (float)iOffset[k] * FXOS8700_UTPERCOUNT;
	}
}





// 10 element calibration using direct eigen-decomposition
static void fUpdateCalibration10EIG(MagCalibration_t *MagCal)
{
	float det;					// matrix determinant
	float fscaling;				// set to FUTPERCOUNT * FMATRIXSCALING
	float ftmp;					// scratch variable
	int16_t iOffset[3];			// offset to remove large DC hard iron bias in matrix
	int16_t iCount;				// number of measurements counted
	int i, j, k, m, n;			// loop counters

	// compute fscaling to reduce multiplications later
	fscaling = FXOS8700_UTPERCOUNT / DEFAULTB;

	// the offsets are guaranteed to be set from the first element but to avoid compiler error
	iOffset[X] = iOffset[Y] = iOffset[Z] = 0;

	// zero the on and above diagonal elements of the 10x10 symmetric measurement matrix fmatA
	for (m = 0; m < 10; m++) {
		for (n = m; n < 10; n++) {
			MagCal->fmatA[m][n] = 0.0F;
		}
	}

	// sum between MINEQUATIONS to MAXEQUATIONS entries into the 10x10 product matrix fmatA
	iCount = 0;
	for (j = 0; j < MAGBUFFSIZE; j++) {
		if (MagCal->valid[j] != -1) {
			// use first valid magnetic buffer entry as estimate for offset
			// to help solution (bit counts)
			if (iCount == 0) {
				for (k = X; k <= Z; k++) {
					iOffset[k] = MagCal->iBpFast[k][j];
				}
			}

			// apply the fixed offset and scaling and enter into fvecA[6-8]
			for (k = X; k <= Z; k++) {
				MagCal->fvecA[k + 6] = (float)((int32_t)MagCal->iBpFast[k][j]
					- (int32_t)iOffset[k]) * fscaling;
			}

			// compute measurement vector elements fvecA[0-5] from fvecA[6-8]
			MagCal->fvecA[0] = MagCal->fvecA[6] * MagCal->fvecA[6];
			MagCal->fvecA[1] = 2.0F * MagCal->fvecA[6] * MagCal->fvecA[7];
			MagCal->fvecA[2] = 2.0F * MagCal->fvecA[6] * MagCal->fvecA[8];
			MagCal->fvecA[3] = MagCal->fvecA[7] * MagCal->fvecA[7];
			MagCal->fvecA[4] = 2.0F * MagCal->fvecA[7] * MagCal->fvecA[8];
			MagCal->fvecA[5] = MagCal->fvecA[8] * MagCal->fvecA[8];

			// accumulate the on-and above-diagonal terms of fmatA=Sigma{fvecA^T * fvecA}
			// with the exception of fmatA[9][9] which equals the number of measurements
			// update the right hand column [9] of fmatA[0-8][9] ignoring fmatA[9][9]
			for (m = 0; m < 9; m++) {
				MagCal->fmatA[m][9] += MagCal->fvecA[m];
			}
			// update the on and above diagonal terms of fmatA ignoring right hand column 9
			for (m = 0; m < 9; m++) {
				for (n = m; n < 9; n++) {
					MagCal->fmatA[m][n] += MagCal->fvecA[m] * MagCal->fvecA[n];
				}
			}

			// increment the measurement counter for the next iteration
			iCount++;
		}
	}

	// set the last element fmatA[9][9] to the number of measurements
	MagCal->fmatA[9][9] = (float) iCount;

	// store the number of measurements accumulated
	MagCal->iMagBufferCount = iCount;

	// copy the above diagonal elements of symmetric product matrix fmatA to below the diagonal
	for (m = 1; m < 10; m++) {
		for (n = 0; n < m; n++) {
			MagCal->fmatA[m][n] = MagCal->fmatA[n][m];
		}
	}

	// set MagCal->fvecA to the unsorted eigenvalues and fmatB to the unsorted
	// normalized eigenvectors of fmatA
	eigencompute(MagCal->fmatA, MagCal->fvecA, MagCal->fmatB, 10);

	// set ellipsoid matrix A from elements of the solution vector column j with
	// smallest eigenvalue
	j = 0;
	for (i = 1; i < 10; i++) {
		if (MagCal->fvecA[i] < MagCal->fvecA[j]) {
			j = i;
		}
	}
	MagCal->fA[0][0] = MagCal->fmatB[0][j];
	MagCal->fA[0][1] = MagCal->fA[1][0] = MagCal->fmatB[1][j];
	MagCal->fA[0][2] = MagCal->fA[2][0] = MagCal->fmatB[2][j];
	MagCal->fA[1][1] = MagCal->fmatB[3][j];
	MagCal->fA[1][2] = MagCal->fA[2][1] = MagCal->fmatB[4][j];
	MagCal->fA[2][2] = MagCal->fmatB[5][j];

	// negate entire solution if A has negative determinant
	det = f3x3matrixDetA(MagCal->fA);
	if (det < 0.0F) {
		f3x3matrixAeqMinusA(MagCal->fA);
		MagCal->fmatB[6][j] = -MagCal->fmatB[6][j];
		MagCal->fmatB[7][j] = -MagCal->fmatB[7][j];
		MagCal->fmatB[8][j] = -MagCal->fmatB[8][j];
		MagCal->fmatB[9][j] = -MagCal->fmatB[9][j];
		det = -det;
	}

	// compute the inverse of the ellipsoid matrix
	f3x3matrixAeqInvSymB(MagCal->finvA, MagCal->fA);

	// compute the trial hard iron vector in offset bit counts times FMATRIXSCALING
	for (k = X; k <= Z; k++) {
		MagCal->ftrV[k] = 0.0F;
		for (m = X; m <= Z; m++) {
			MagCal->ftrV[k] += MagCal->finvA[k][m] * MagCal->fmatB[m + 6][j];
		}
		MagCal->ftrV[k] *= -0.5F;
	}

	// compute the trial geomagnetic field strength B in bit counts times FMATRIXSCALING
	MagCal->ftrB = sqrtf(fabs(MagCal->fA[0][0] * MagCal->ftrV[X] * MagCal->ftrV[X] +
			2.0F * MagCal->fA[0][1] * MagCal->ftrV[X] * MagCal->ftrV[Y] +
			2.0F * MagCal->fA[0][2] * MagCal->ftrV[X] * MagCal->ftrV[Z] +
			MagCal->fA[1][1] * MagCal->ftrV[Y] * MagCal->ftrV[Y] +
			2.0F * MagCal->fA[1][2] * MagCal->ftrV[Y] * MagCal->ftrV[Z] +
			MagCal->fA[2][2] * MagCal->ftrV[Z] * MagCal->ftrV[Z] - MagCal->fmatB[9][j]));

	// calculate the trial normalized fit error as a percentage
	MagCal->ftrFitErrorpc = 50.0F * sqrtf(
		fabs(MagCal->fvecA[j]) / (float) MagCal->iMagBufferCount) /
		(MagCal->ftrB * MagCal->ftrB);

	// correct for the measurement matrix offset and scaling and
	// get the computed hard iron offset in uT
	for (k = X; k <= Z; k++) {
		MagCal->ftrV[k] = MagCal->ftrV[k] * DEFAULTB + (float)iOffset[k] * FXOS8700_UTPERCOUNT;
	}

	// convert the trial geomagnetic field strength B into uT for
	// un-normalized soft iron matrix A
	MagCal->ftrB *= DEFAULTB;

	// normalize the ellipsoid matrix A to unit determinant and
	// correct B by root of this multiplicative factor
	f3x3matrixAeqAxScalar(MagCal->fA, powf(det, -(ONETHIRD)));
	MagCal->ftrB *= powf(det, -(ONESIXTH));

	// compute trial invW from the square root of fA (both with normalized determinant)	
	// set fvecA to the unsorted eigenvalues and fmatB to the unsorted eigenvectors of fmatA
	// where fmatA holds the 3x3 matrix fA in its top left elements
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			MagCal->fmatA[i][j] = MagCal->fA[i][j];
		}
	}
	eigencompute(MagCal->fmatA, MagCal->fvecA, MagCal->fmatB, 3);

	// set MagCal->fmatB to be eigenvectors . diag(sqrt(sqrt(eigenvalues))) =
	//   fmatB . diag(sqrt(sqrt(fvecA))
	for (j = 0; j < 3; j++) { // loop over columns j
		ftmp = sqrtf(sqrtf(fabs(MagCal->fvecA[j])));
		for (i = 0; i < 3; i++) { // loop over rows i
			MagCal->fmatB[i][j] *= ftmp;
		}
	}

	// set ftrinvW to eigenvectors * diag(sqrt(eigenvalues)) * eigenvectors^T =
	//   fmatB * fmatB^T = sqrt(fA) (guaranteed symmetric)
	// loop over rows
	for (i = 0; i < 3; i++) {
		// loop over on and above diagonal columns
		for (j = i; j < 3; j++) {
			MagCal->ftrinvW[i][j] = 0.0F;
			// accumulate the matrix product
			for (k = 0; k < 3; k++) {
				MagCal->ftrinvW[i][j] += MagCal->fmatB[i][k] * MagCal->fmatB[j][k];
			}
			// copy to below diagonal element
			MagCal->ftrinvW[j][i] = MagCal->ftrinvW[i][j];
		}
	}
}





