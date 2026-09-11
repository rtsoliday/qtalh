# Engineering history

These records preserve the measurements, observations, and implementation decisions behind the Qt port. Read their dates and environments before applying a result to another installation.

| Record | What it establishes |
| --- | --- |
| [CPU benchmarks](./performance) | Engine/UI and local-IOC comparisons, with raw trial data and workload descriptions. |
| [Logging investigation](./logging) | The old circular-log overhead and its later replacement with persistent binary checkpoints. |
| [Appearance comparisons](./appearance) | Motif/Qt reference captures, auxiliary-window work, and remaining rendering differences. |
| [Compatibility inventory](/understand/compatibility) | Feature evidence, protocol details, and explicit acceptance gaps. |

The historical circular-logging measurements describe an earlier implementation. The later [checkpoint result](./logging#measured-result) reports the optimization without reducing recorded transitions.

## Original documentation

The <a href="/legacy/ALH.html" target="_self">original ALH manual</a> and its images are included as a historical reference. For current Qt-specific options and workflows, use the modern [guide](/get-started/first-run) and [reference](/reference/command-line).
