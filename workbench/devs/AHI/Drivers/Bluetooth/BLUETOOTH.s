
	FORM_START	AHIM

	CHUNK_START	AUDN
	.asciz		"bluetooth"
	CHUNK_END

	/* Generic modes play to the sink named in ENV:Bluetooth/AudioDevice;
	   C:BTAudioModes adds a mode per paired sink, named after the device. */
	CHUNK_START	AUDM
1:
	LONG2		AHIDB_AudioID,	0x00460001
	LONG2		AHIDB_Volume,	TRUE
	LONG2		AHIDB_Panning,	FALSE
	LONG2		AHIDB_Stereo,	FALSE
	LONG2		AHIDB_HiFi,	FALSE
	LONG2		AHIDB_MultTable,FALSE
	LONG2		AHIDB_Name,	2f-1b
	LONG		TAG_DONE
2:
	.asciz		"BLUETOOTH:16 bit mono"
	CHUNK_END

	CHUNK_START	AUDM
1:
	LONG2		AHIDB_AudioID,	0x00460002
	LONG2		AHIDB_Volume,	TRUE
	LONG2		AHIDB_Panning,	TRUE
	LONG2		AHIDB_Stereo,	TRUE
	LONG2		AHIDB_HiFi,	FALSE
	LONG2		AHIDB_MultTable,FALSE
	LONG2		AHIDB_Name,	2f-1b
	LONG		TAG_DONE
2:
	.asciz		"BLUETOOTH:16 bit stereo++"
	CHUNK_END

	FORM_END

	.end
