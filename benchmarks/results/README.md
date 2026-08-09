# Local baseline results

Each invocation creates one exclusive, timestamped run directory containing
`manifest.json`, `samples.csv`, `summary.json`, and a bounded `run.log`.
Everything else is ignored. Results are local evidence only and must not be
committed, especially when they include temporary configuration or model data.

Remove only the exact run directory created for a completed invocation. Do not
recursively delete an unknown output root.
