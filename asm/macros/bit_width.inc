	.macro space64 x:req
		.if VER_64BIT == 1
			.space \x
		.endif
	.endm

	.macro ptrvalue x:vararg
		.if VER_64BIT == 1
			.quad \x
		.else
			.int \x
		.endif
	.endm

	/* BUG-015 (PS-079): the opposite of space64, for fields that must be the same size on both builds. */
	.macro space32 x:req
		.if VER_64BIT != 1
			.space \x
		.endif
	.endm
