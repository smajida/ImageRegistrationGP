/*
  fft2d_unit.c

  Copyright (C) 2016 Jukka Soikkeli and Chris Smallwood
  Copyright (C) 2012 Adapteva, Inc.
  Contributed by Yainv Sapir <yaniv@adapteva.com>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program, see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>.
*/


// This program is the accelerator part of the 2D-FFT project.
//
// This program runs on the Epiphany system and answers the host with the
// calculation result of the operand matrices.
//
// Jan-2012, YS.


#include <e-lib.h>
#include "fft2dlib.h"
#include "fft2d.h"
#include "dmalib.h"
#include "dram_buffers.h"
#include "static_buffers.h"

/*
void FFT2D(fft_dir_t dir); //TO BE REMOVED
void corner_turn(int pingpong); //TO BE REMOVED
void LPF(int lgNN); //TO BE REMOVED
*/

void init();
void calc();


///////////////////////////////////////////////////////
///////////////////////////////////////////////////////
int main(int argc, char *argv[])
{
	int status;

	status = 0;

	// Initialize data structures - mainly target pointers 
	dstate(1);  //CHECK - debugging info - REMOVE? [remove all dstate parts, not needed for us]
	init();
	dstate(2);

	do {
		dstate(3);
		if (me.corenum == 0)
		{
			// Wait for fft() call from the host. When a rising
			// edge is detected in the mailbox, the loop
			// is terminated and a call to the actual
			// FFT() function is initiated.
		        while (Mailbox.pCore->go == 0) {};  //busy waiting loop for host to signal "go"

			e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);
			me.time_p[0] = e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);


			Mailbox.pCore->ready = 0;

			me.go_sync = (e_group_config.group_rows * e_group_config.group_cols) + 1;
		} else {
			// Wait for "go" from the previous core. When a rising
			// edge is detected in the core's mailbox, the loop
			// is terminated and a call to the actual
			// FFT() function is initiated.
			while (me.go_sync == 0) {};
		}
		// Signal "go" to next core.
		dstate(4);
		*me.tgt_go_sync = me.corenum + 1;

		// Load _Score rows from DRAM.
#ifdef _USE_DRAM_
#	warning "Using DRAM for I/O storage"
#	ifdef _USE_DMA_E_
		dmacpye((void *) &(Mailbox.pA[me.corenum * _Score * _Sfft]), me.bank[_BankA][_PING]);
#	else // _USE_DMA_E_
#		warning "Using rowcpy() instead of DMA_E"
		rowcpy(&(Mailbox.pA[me.corenum * _Score * _Sfft]), me.bank[_BankA][_PING], _Score * _Sfft);
#	endif // _USE_DMA_E_
#endif // _USE_DRAM_

		// ======= OUR CALCULATIONS ========== //
		me.time_p[1] = e_ctimer_get(E_CTIMER_0); //CHECK!!
		calc();  // function for calculations
		me.time_p[2] = e_ctimer_get(E_CTIMER_0); //CHECK!!

		/*
		//OLD "CALCULATE" parts
		dstate(5);
		
		// Calculate. During this time, the host polls the
		// Core 0's mailbox, waiting for a falling
		// edge indicating the end of the calculation.   //WHAT is a falling edge?
		me.time_p[1] = e_ctimer_get(E_CTIMER_0);
		FFT2D(e_fft_fwd);

		me.time_p[6] = e_ctimer_get(E_CTIMER_0);
		LPF(_lgSfft);

		me.time_p[7] = e_ctimer_get(E_CTIMER_0);
		FFT2D(e_fft_bwd);

		me.time_p[8] = e_ctimer_get(E_CTIMER_0);
		dstate(6);
		*/

		// Save _Score rows to DRAM.
#ifdef _USE_DRAM_
#	ifdef _USE_DMA_E_
		dmacpye(me.bank[_BankA][_PING], (void *) &(Mailbox.pB[me.corenum * _Score * _Sfft]));
#	else // _USE_DMA_E_
#		warning "Using rowcpy() instead of DMA_E"
		rowcpy(me.bank[_BankA][_PING], &(Mailbox.pB[me.corenum * _Score * _Sfft]), _Score * _Sfft);
#	endif // _USE_DMA_E_
#endif // _USE_DRAM_

		// If this is the first core, wait until all cores finished calculation and signal the host.
		dstate(7);
		if (me.corenum == 0)
		{
			while (me.go_sync == ((e_group_config.group_rows * e_group_config.group_cols) + 1)) {};
			// Signal own End-Of-Calculation to previous core.
			me.go_sync = 0;
	        // Wait until next core ends calculation.
			dstate(8);
			while (*me.tgt_go_sync > 0) {};
			dstate(9);

			me.time_p[9] = e_ctimer_stop(E_CTIMER_0);
//			me.time_p[0] = 100;
//			me.time_p[1] = 101;
//			me.time_p[2] = 102;
//			me.time_p[9] = 109;

			Mailbox.pCore->ready = 1;
			Mailbox.pCore->go = 0;
			dstate(10);
		} else {
	        // If next core ended calculation, signal own End-Of-Calculation to previous core.
			dstate(11);
			while (*me.tgt_go_sync > 0) {};
			dstate(12);
			me.go_sync = 0;
			dstate(13);
		}
	} while (0);

	return status;
}




void calc() {
  	int i0, i1, j, l, l1, l2, N, Wc;
	cfloat t;
	cfloat *X;
	cfloat *W;

	int row=0;
	int Wn_offset = 0;
	
	/*
	for(row=0;row<_Score;row++) { 
	  volatile cfloat * restrict _X = (me.bank[_BankA][_PING] + row *_Sfft);
	  volatile cfloat * restrict _W = me.bank[_BankW][_PING]+Wn_offset;
	  
	  X = __builtin_assume_aligned((void *) _X, 8);
	  W = __builtin_assume_aligned((void *) _W, 8);
	  
	  // Calculate the number of points
	  N = 1 << 2;//1 << lgN;
	  
	  //Loop for changing values
	  for (i0=0; i0<N; i0++)
	      {
		X[i0] = X[i0] * 2;
	      }
	  
	  //bitrev(me.bank[_BankA][_PONG], _lgSfft, _Score);
	  //corner_turn(_PING);

	  for (i0=0; i0<N; i0++)
	    {
	      X[i0] = X[i0] * 0;
	    }
	  
	}
	*/
    
	for(row=0;row<_Score;row++) { 
	  volatile cfloat * restrict xX = (me.bank[_BankA][_PING] + row *_Sfft);

	  for(int col=0;col<_Sfft;col++) { //change the name _Sfft to something else
	    //xX[col] *= 0.5;

	    //float mult = 0.5;
	    //Mailbox.pCore->mult = 0.5;
	    float mult = Mailbox.pCore->mult;
	    //float mult;
	    //if(Mailbox.pCore->mult != 0)
	    //  mult = Mailbox.pCore->mult;
	    //else
	    //  mult = 1.5;
	    int limit  = 255; //limit for image value (255, as 8-bit image elements)
	    int newval = (int) (xX[col]*mult); //value after multiplication
	    if(newval <= limit) //if newval is less than limit, apply
	      xX[col] *= mult;
	    else                //if newval is beyond limit, assign limit value (not enough bits for more)
	      xX[col] = limit;
	  }
	}	

	return;

}

///////////////////////////////////////////////////////
///////////////////////////////////////////////////////
/*
void FFT2D(fft_dir_t dir)  //TO BE REMOVED
{
	int row, cnum, Wn_offset;

	if (dir == e_fft_fwd)
		Wn_offset = 0;
	else
		Wn_offset = _Sfft >> 1;

	dstate(100);
	for (cnum=0; cnum<_Ncores; cnum++)
		me.sync[cnum] = 0;

	// Reorder vectors w/ bit reversal
	me.time_p[2] = e_ctimer_get(E_CTIMER_0);
	bitrev(me.bank[_BankA][_PING], _lgSfft, _Score);

	// Perform 1D-FFT on _Score rows
	me.time_p[3] = e_ctimer_get(E_CTIMER_0);
	for (row=0; row<_Score; row++)
		fft_1d_r2_dit(_lgSfft, (me.bank[_BankA][_PING] + row * _Sfft), me.bank[_BankW][_PING]+Wn_offset, _Sfft);

	dstate(101);
	// Do the corner turn
	me.time_p[4] = e_ctimer_get(E_CTIMER_0);
	corner_turn(_PING);
	me.time_p[5] = e_ctimer_get(E_CTIMER_0);

	dstate(102);
	// Signal for sync
	for (cnum=0; cnum<_Ncores; cnum++)
		*me.tgt_sync[cnum] = 1;

	// Wait for sync from all cores
	for (cnum=0; cnum<_Ncores; cnum++)
		while (me.sync[cnum] < 1) {};

	// Reorder vectors w/ bit reversal
	dstate(103);
	bitrev(me.bank[_BankA][_PONG], _lgSfft, _Score);

	// Perform 1D-FFT on _Score rows
	for (row=0; row<_Score; row++)
		fft_1d_r2_dit(_lgSfft, (me.bank[_BankA][_PONG] + row * _Sfft), me.bank[_BankW][_PING]+Wn_offset, _Sfft);

	dstate(104);
	// Do the corner turn
	corner_turn(_PONG);

	// Signal for sync
	dstate(105);
	for (cnum=0; cnum<_Ncores; cnum++)
		*me.tgt_sync[cnum] = 2;

	// Wait for sync from all cores
	for (cnum=0; cnum<_Ncores; cnum++)
		while (me.sync[cnum] < 2) {};

	dstate(106);
	return;
}
*/

///////////////////////////////////////////////////////
///////////////////////////////////////////////////////
/*
void corner_turn(int pingpong)   //TO BE REMOVED
{
#ifdef _USE_DMA_I_
	unsigned cnum;

	for (cnum=0; cnum<_Ncores; cnum++)
	{
		dstate(200 + cnum);
		dmacpyi((void *) (me.bank[_BankA][pingpong] + _Score * cnum), (void *) (me.tgt_bk[cnum][_BankA][pingpong] + _Score * me.corenum));
	}




#else
#	warning "Using memcpy() instead of DMA_I"
	unsigned row, col, cnum;

	// Transpose cores
	for (cnum=0; cnum<_Ncores; cnum++)
	{
		for (row=0; row<_Score; row++)
		{
			for (col=0; col<_Score; col++)
			{
				*(me.tgt_bk[cnum][_BankA][pingpong] + _Sfft * col + _Score * me.corenum + row) =
				        *(me.bank[_BankA][pingpong] + _Sfft * row + _Score * cnum       + col);
			}
		}
	}
#endif

	return;
}
*/

///////////////////////////////////////////////////////
///////////////////////////////////////////////////////
/*
void LPF(int lgNN)   //TO BE REMOVED
{
	int row, col, k;
	#define Fco 2

	if (me.corenum < (8-Fco)*(_Ncores >> 4))
	{
		for (row=0, k=0; row<_Score; row++)
		{
			for (col=0; col<((8-Fco)*(_Sfft>>4)); col++)
				me.bank[_BankA][_PING][k++] *= recipro_2_by[lgNN+lgNN];
			for (     ; col<((8+Fco)*(_Sfft>>4)); col++)
				me.bank[_BankA][_PING][k++] = 0;
			for (     ; col<((8+8)*(_Sfft>>4)); col++)
				me.bank[_BankA][_PING][k++] *= recipro_2_by[lgNN+lgNN];
		}
	}
	else if (me.corenum < (8+Fco)*(_Ncores >> 4))
	{
		for (k=0; k<(_Score * _Sfft); )
			me.bank[_BankA][_PING][k++] = 0;
	}
	else
	{
		for (row=0, k=0; row<_Score; row++)
		{
			for (col=0; col<((8-Fco)*(_Sfft>>4)); col++)
				me.bank[_BankA][_PING][k++] *= recipro_2_by[lgNN+lgNN];
			for (     ; col<((8+Fco)*(_Sfft>>4)); col++)
				me.bank[_BankA][_PING][k++] = 0;
			for (     ; col<((8+8)*(_Sfft>>4)); col++)
				me.bank[_BankA][_PING][k++] *= recipro_2_by[lgNN+lgNN];
		}
	}

	return;
}
*/

///////////////////////////////////////////////////////
///////////////////////////////////////////////////////
void init()
{
	int row, col, cnum;

	// Initialize the mailbox shared buffer pointers
	Mailbox.pBase = (void *) SHARED_DRAM;
	Mailbox.pA    = Mailbox.pBase + offsetof(shared_buf_t, A[0]);
	Mailbox.pB    = Mailbox.pBase + offsetof(shared_buf_t, B[0]);
	Mailbox.pCore = Mailbox.pBase + offsetof(shared_buf_t, core);

	// Initialize per-core parameters - core data structure
	// Use eLib's e_coreid library's API to retrieve core specific information
	// Use the predefined constants to determine the relative coordinates of the core
	me.row = e_group_config.core_row;
	me.col = e_group_config.core_col;
	me.corenum = me.row * e_group_config.group_rows + me.col;

	// Initialize pointers to the operand matrices ping-pong arrays
	me.bank[_BankA][_PING] = (cfloat *) &(AA[0][0]);
	me.bank[_BankA][_PONG] = (cfloat *) &(BB[0][0]);
	me.bank[_BankW][_PING] = (cfloat *) &(Wn[0]);

	// Use the e_neighbor_id() API to generate the pointer addresses of the arrays
	// in the horizontal and vertical target cores, where the submatrices data will
	// be swapped.
	// CHECK - but likely needed anyway...
	cnum = 0;
	for (row=0; row<e_group_config.group_rows; row++)
		for (col=0; col<e_group_config.group_cols; col++)
		{
			me.tgt_bk[cnum][_BankA][_PING] = e_get_global_address(row, col, (void *) me.bank[_BankA][_PONG]);
			me.tgt_bk[cnum][_BankA][_PONG] = e_get_global_address(row, col, (void *) me.bank[_BankA][_PING]);
			me.tgt_sync[cnum]              = e_get_global_address(row, col, (void *) (&me.sync[me.corenum]));
			cnum++;
		}
	e_neighbor_id(E_NEXT_CORE, E_GROUP_WRAP, &row, &col);
	me.tgt_go_sync = e_get_global_address(row, col, (void *) (&me.go_sync));

	// Generate Wn
	if(_lgSfft == 6) {
	generateWn(me.bank[_BankW][_PING], 7);  //TEMP HARDCODED - for testing _lgSfft=6 issues...
	}
	else {
	generateWn(me.bank[_BankW][_PING], _lgSfft);  //CHECK - what do we do with _lgSfft, or its equivalent?
	}


	// Clear the inter-core sync signals
	me.go_sync = 0;
	for (cnum=0; cnum<_Ncores; cnum++)
		me.sync[cnum] = 0;

	// Init the host-accelerator sync signals
	Mailbox.pCore->go = 0;
	Mailbox.pCore->ready = 1;
	//Mailbox.pCore->mult = 1;

#if 0
	// Initialize input image
	for (row=0; row<_Score; row++)
	{
		for (col=0; col<_Sfft; col++)
			*(me.bank[_BankA][_PING] + row * _Sfft + col) = (me.corenum * _Score + row)*1000.0 + col;
		// convert to eDMA
		rowcpy((me.bank[_BankA][_PING] + row * _Sfft), &(Mailbox.pA[(me.corenum * _Score + row) * _Sfft]), _Sfft);
	}
#endif

	return;
}
