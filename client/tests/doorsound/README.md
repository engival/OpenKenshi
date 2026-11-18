# doorsound (kenshi-h2v.29.8)

`run.sh OUT` runs bar_lure headless with `KAUDIO_WAV`: the Wanderer walks into the Hub bar, the door (id 41048) is closed and opened with `interact id=41048 2|1`,
the Wanderer walks out. Expected in OUT/client.log (and printed): `combat sound: ... Door_Open / Door_Shut / Door_Close` with `Door_Type=Wood`,
`audio: Enter_Building Indoors=Wicker Building_Integrity=70` on entry and `audio: Leave_Building` on exit. `../carrysound/run.sh` (slavers_downed) shows
`Character_Pick_Up_Person` when the caravan carries the Wanderer.
