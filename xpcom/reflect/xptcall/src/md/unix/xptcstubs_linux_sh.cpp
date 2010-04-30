/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* ***** BEGIN LICENSE BLOCK *****
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
 * Portions created by the Initial Developer are Copyright (C) 1999
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
 * Based on the neutrino code, with some bug fixes and using the C preprocessor
 * like all the other ports rather than the python script.
 *
 * ***** END LICENSE BLOCK ***** */

#include "xptcprivate.h"
#include "xptiprivate.h"

const int c_int_register_params = 3;
const int c_float_register_params = 8;

/*
   Dispatch function for all stubs.

   The parameters to the original function are spread between 'data' which
   is value of the stack pointer when the stub was called, intRegParams which
   points to an area containing the values of r5, r6 and r7 when the stub was
   called and floatRegParams which points to an area containing the values
   of float registers fr4 to fr11 when the stub was called.

 */
extern "C" nsresult
PrepareAndDispatch(nsXPTCStubBase* self, int methodIndex, PRUint32* data,
				   PRUint32 *intRegParams, float *floatRegParams)
{
#define PARAM_BUFFER_COUNT     16

	nsresult result = NS_ERROR_FAILURE;
	int intCount = 0;
	int floatCount = 0;
	nsXPTCMiniVariant paramBuffer[PARAM_BUFFER_COUNT];
	nsXPTCMiniVariant* dispatchParams = NULL;
	const nsXPTMethodInfo* info;
	PRUint8 paramCount;
	PRUint8 i;

	NS_ASSERTION(self,"no self");

	self->mEntry->GetMethodInfo(PRUint16(methodIndex), &info);
	NS_ASSERTION(info,"no interface info");

	paramCount = info->GetParamCount();

	// setup variant array pointer
	if(paramCount > PARAM_BUFFER_COUNT)
		dispatchParams = new nsXPTCMiniVariant[paramCount];
	else
		dispatchParams = paramBuffer;
	NS_ASSERTION(dispatchParams,"no place for params");

	for ( i = 0; i < paramCount; ++i ) {
		const nsXPTParamInfo& param = info->GetParam(i);
		nsXPTCMiniVariant* dp = &dispatchParams[i];
		nsXPTType type = param.IsOut() ? nsXPTType::T_I32 : param.GetType();

		switch ( type ) {
			case nsXPTType::T_I64:
			case nsXPTType::T_U64:
				// Was this passed  in a register?
				if ( (c_int_register_params - intCount) >= 2 ) {
					dp->val.i64 = *((PRInt64 *) intRegParams);
					intRegParams += 2;
					intCount += 2;
				}
				else {
					dp->val.i64 = *((PRInt64*) data);
					data += 2;
				}
				break;
			case nsXPTType::T_FLOAT:
				// Was this passed  in a register?
				if ( floatCount < c_float_register_params ) {
					dp->val.f = *floatRegParams;
					++floatCount;
					++floatRegParams;
				}
				else {
					dp->val.f = *((float*) data);
					++data;
				}
				break;
			case nsXPTType::T_DOUBLE:
				// Was this passed  in a register?
				if ( (c_float_register_params - floatCount) >= 2  ) {
					if ( floatCount & 1 != 0 ) {
						++floatCount;
						++floatRegParams;
					}
					dp->val.d = *(double *)floatRegParams;
					floatCount += 2;
					floatRegParams += 2;
				}
				else {
					dp->val.d = *((double *) data);
					data += 2;
				}
				break;
			default:		// 32-bit (non-float) value
				// Was this passed  in a register?
				if ( intCount < c_int_register_params ) {
					dp->val.i32 = *intRegParams;
					++intRegParams;
					++intCount;
				}
				else {
					dp->val.i32 = *data;
					++data;
				}
				break;
		}
	}

	result = self->mOuter->CallMethod((PRUint16)methodIndex, info, dispatchParams);

	if(dispatchParams != paramBuffer)
		delete [] dispatchParams;

	return result;
}


__asm__ (
	".text\n"
	".little\n"
	".section	.rodata\n"

	".globl SharedStub\n"
	".type  SharedStub, @function\n"
	"SharedStub:\n"
	"mov	   r15, r1\n"
	"mov.l	r14,@-r15\n"
	"sts.l	pr,@-r15\n"
	"mov		r15, r14\n"

	/* Some parameters might have been passed in registers, so push them
	 * all onto the stack, PrepareAndDispatch can then work out whats what
	 * given method type information.
	 */
	"mov.l r7, @-r15\n"
	"mov.l r6, @-r15\n"
	"mov.l r5, @-r15\n"
	"mov	  r15, r7\n"	/* r7 = PrepareAndDispatch intRegParams param	*/

	"fmov.s fr10, @-r15\n"
	"fmov.s fr11, @-r15\n"
	"fmov.s fr8, @-r15\n"
	"fmov.s fr9, @-r15\n"
	"fmov.s fr6, @-r15\n"
	"fmov.s fr7, @-r15\n"
	"fmov.s fr4, @-r15\n"
	"fmov.s fr5, @-r15\n"
	"mov.l  r15, @-r15\n"	/* PrepareAndDispatch floatRegParams param		*/

	"mov	   r1, r6\n"	/* r6 = PrepareAndDispatch data param			*/

	"mov.l  1f, r1\n"
	"jsr	   @r1\n"		/* Note, following instruction is executed first*/
	  "mov	   r2, r5\n"		/* r5 = PrepareAndDispatch methodIndex param	*/

	"mov		r14,r15\n"
	"lds.l	@r15+,pr\n"
	"mov.l	@r15+,r14\n"
	"rts\n"
	  "nop\n"
	".align 2\n"
	"1:\n"
	".long  PrepareAndDispatch\n"
	);

#define STUB_ENTRY(n) 						\
__asm__( 							\
	".text\n"						\
	".align 1 \n"						\
	".if        " #n " < 10\n\t" 				\
	".globl    _ZN14nsXPTCStubBase5Stub" #n "Ev\n\t" 	\
	".type     _ZN14nsXPTCStubBase5Stub" #n "Ev,@function\n"\
	"_ZN14nsXPTCStubBase5Stub" #n "Ev:\n\t" 		\
	".elseif    " #n " < 100\n\t" 				\
	".globl    _ZN14nsXPTCStubBase6Stub" #n "Ev\n\t" 	\
	".type     _ZN14nsXPTCStubBase6Stub" #n "Ev,@function\n"\
	"_ZN14nsXPTCStubBase6Stub" #n "Ev:\n\t" 		\
	".elseif    " #n " < 1000\n\t" 				\
	".globl    _ZN14nsXPTCStubBase7Stub" #n "Ev\n\t" 	\
	".type     _ZN14nsXPTCStubBase7Stub" #n "Ev,@function\n"\
	"_ZN14nsXPTCStubBase7Stub" #n "Ev:\n\t" 		\
	".else\n\t" 						\
	".err       \"stub number " #n " >= 1000 not yet supported\"\n\t" \
	".endif\n\t" 						\
	"mov.l 	1f, r1 \n"					\
	".if "#n" < 128 \n"					\
	"jmp @r1 \n"						\
	"  mov #"#n",r2 \n"					\
	".elseif "#n" < 256 \n"					\
	"mov #"#n", r2 \n"					\
	"jmp @r1 \n"						\
	"  extu.b r2, r2 \n"					\
	".else \n"						\
	"mov #"#n" & 0xff,r2 \n"				\
	"extu.b	r2, r2 \n"					\
	"mov #"#n">>8, r3 \n"					\
	"shll8	r3 \n"						\
	"jmp @r1 \n"						\
	"  or r3, r2 \n"					\
	".endif \n"						\
	".if "#n" % 20 == 0\n"					\
	".align 2\n"						\
	"1:\n"							\
	".long SharedStub\n"					\
	".endif\n"						\
	);


/* Due to the fact that the SH4 can only load forward labels, we have
 * to use sentinel_entry to output the last label. A better solution
 * would be to introduce a STUB_LAST macro in the defs.in file, but
 * this will do for now
 */

#define SENTINEL_ENTRY(n) 		\
__asm__( 				\
".if "#n" == 0  \n"			\
	".text \n"			\
	".align 2\n"			\
	"1:\n"				\
	".long SharedStub\n"		\
".endif\n"				\
); 					\
					\
nsresult nsXPTCStubBase::Sentinel##n()  \
{ \
	NS_ASSERTION(0,"nsXPTCStubBase::Sentinel called"); \
	return NS_ERROR_NOT_IMPLEMENTED; \
}

#include "xptcstubsdef.inc"
