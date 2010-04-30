/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  - Copyright (C) 2008-2009 STMicroelectronics
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * Based on the neutroni port, however that assumed that the compiler was pushing params
 * onto the stack. Change to take this into account.
 *
 * ***** END LICENSE BLOCK ***** */

#include "xptcprivate.h"

extern "C" {

const int c_int_register_params = 4;
const int c_float_register_params = 8;

static PRUint32 __attribute__((__used__))
copy_to_stack(PRUint32 **that,PRUint32 methodIndex,PRUint32 paramCount, nsXPTCVariant* s,PRUint32* data)
{
	int intCount = 1; // Because of that
	int floatCount = 0;
	PRUint32 *intRegParams=data+1 ;
	float  *floatRegParams = (float *)(data+4);

	/* Push the that register into the right place so it can be restored on exit */
	*data= (PRUint32)(that);
	data+=12; /* 4 integer registers, and 8 floating point registers */

	for ( PRUint32 i = 0; i < paramCount; ++i, ++s )
	{
		nsXPTType type = s->IsPtrData() ? nsXPTType::T_I32 : s->type;

		switch ( type ) {
			case nsXPTType::T_I64:
			case nsXPTType::T_U64:
				// Space to pass in registers?
				if ( (c_int_register_params - intCount) >= 2 ) {
					*((PRInt64 *) intRegParams) = s->val.i64;
					intRegParams += 2;
					intCount += 2;
				}
				else {
					*((PRInt64*) data) = s->val.i64;
					data += 2;
				}
				break;
			case nsXPTType::T_FLOAT:
				// Space to pass in registers?
				if ( floatCount < c_float_register_params ) {
					*floatRegParams = s->val.f;
					++floatCount;
					++floatRegParams;
				}
				else {
					*((float*) data) = s->val.f;
					++data;
				}
				break;
			case nsXPTType::T_DOUBLE:
				// Space to pass in registers?
				if ( (c_float_register_params - floatCount) >= 2  ) {
					if ( (floatCount & 1) != 0 ) {
						++floatCount;
						++floatRegParams;
					}
					*(double *)floatRegParams = s->val.d;
					floatCount += 2;
					floatRegParams += 2;
				}
				else {
					*((double *) data) = s->val.d;
					data += 2;
				}
				break;
			default:		// 32 (non-float) value
				PRInt32 value = (PRInt32) (s->IsPtrData() ?  s->ptr : s->val.p);
				// Space to pass in registers?
				if ( intCount < c_int_register_params ) {
					*intRegParams = value;
					++intRegParams;
					++intCount;
				}
				else {
					*data = value;
					++data;
				}
				break;
		}
	}

	/* Now calculate the return address
	 * Dereference that to get vtable pointer
	 */
	return *( (*(that))+(methodIndex) );

}

}

	/* This was originally done as a C function, but the original code was
	 * relying on how the compiler laid out the stack. Later versions of
	 * gcc do a better job of optimising and never push the parameters on the
	 * stack. So it is simpler to just write the whole thing in assembler anyway
	 */

	/* Because the SH processor passes the first few parameters in registers
	   it is a bit tricky setting things up right.  To make things easier,
	   all the hard work will be done by copy_to_stack above.  We pass to it
	   a chunk of memory, the bottom of which will be copied to registers r4 to r7
	   and fr4 to fr11 before calling the target function.
	*/

/* r4= that, r5=methodIndex,r6=paramCount,r7=params */

 __asm__ (


    /* Make space for parameters to be passed to the method.  Assume worst case
       8 bytes per parameter.  Also leave space for 4 longs and 8 floats that
       will be put into registers.  The worst case is all int64 parameters
       and even in this case 8 bytes are passed in registers so we can
       deduct this from our allocation.
    */
	".section .text\n"
	".balign 4\n"
	".global NS_InvokeByIndex_P\n"
	"NS_InvokeByIndex_P:\n"
	"mov.l r14, @-r15 \n\t" // Push frame
	"sts.l pr, @-r15 \n\t"	// Push link
	"mov.l r8, @-r15 \n\t"	// Save
	"mov  r15, r14\n\t"	// Set frame
	"mov	#3, r1 \n\t" 	// Assume worse case, all params are 64bit, hence *8
	"mov 	r6, r2\n\t"
	"shld	r1, r2 \n\t"
	"mov 	r2, r8  \n\t" 	// Save stack drop
	"add	#48, r2 \n\t"	// Space for 4 longs, 8 floats
	"sub	r2, r15 \n\t"	// Drop stack
	"mov.l 	1f, r1 \n\t"	// Get address of copy_to_stack_function
	"jsr  	@r1 \n\t"
	  "mov.l   r15, @-r15 \n\t"	//  Params will be dumped here
	"add	#4, r15 \n\t"	// Pop stack ptr param. r0 contains method address

	/* Now everything is laid out nicely in the stack.  We just have to
	   load values at the top end of the memory area into registers and
	   make the call.  We may load more things into registers than needed,
	   but nobody will care about that.
	*/

	"mov.l	@r15+, r4 \n\t" // that
	"mov.l	@r15+, r5 \n\t"
	"mov.l	@r15+, r6 \n\t"
	"mov.l	@r15+, r7 \n\t"
	"fmov.s	@r15+, fr5 \n\t"
	"fmov.s	@r15+, fr4 \n\t"
	"fmov.s	@r15+, fr7 \n\t"
	"fmov.s	@r15+, fr6 \n\t"
	"fmov.s	@r15+, fr9 \n\t"
	"fmov.s	@r15+, fr8 \n\t"
	"fmov.s	@r15+, fr11 \n\t"
	"jsr	@r0 \n\t" // Invoke method
	  "fmov.s	@r15+, fr10 \n\t"
	"add  r8, r15\n\t"		// Pop stack back
	"mov.l @r15+, r8\n\t" // Restore r8
	"lds.l @r15+, pr\n\t"
	"rts\n\t"
	  "mov.l @r15+, r14\n\t"
	".balign 4\n\t"
	"1: .long copy_to_stack \n\t"
   );

