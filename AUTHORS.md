# Authors and acknowledgements

## QtALH

QtALH is the Qt port of the EPICS Alarm Handler and Alarm Configuration Tool.

- **Robert Soliday** — Qt port development and maintenance.

The port adapts the original ALH configuration parsing, alarm algorithms,
operator workflows, and helper protocols. Its implementation is in `qtalh/`;
the original Motif implementation remains in `alh/`.

## Original ALH authors

ALH was developed at Argonne National Laboratory. The original author credits,
preserved in [alh/version.h](alh/version.h), name:

- Ben-Chin Cha
- Janet Anderson
- Mark Anderson
- Marty Kraimer
- Albert Kagarmanov

## Additional ALH contributors

- John Sinclair and Kay Kasemir — SNS modifications, March 2007.
- Andreas Luedeke — PSI modifications, May 2012.

These credits preserve the original ALH attribution and distinguish it from
QtALH development. Existing copyright notices and the [LICENSE](LICENSE)
remain applicable; this author list does not replace them.
