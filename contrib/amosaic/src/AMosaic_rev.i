VERSION		EQU	14
REVISION	EQU	1685

DATE	MACRO
		dc.b '2.4.2002'
		ENDM

VERS	MACRO
		dc.b 'AMosaic 14.1685'
		ENDM

VSTRING	MACRO
		dc.b 'AMosaic 14.1685 (2.4.2002)',13,10,0
		ENDM

VERSTAG	MACRO
		dc.b 0,'$VER: AMosaic 14.1685 (2.4.2002)',0
		ENDM
