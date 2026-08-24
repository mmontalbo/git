# concurrency probe

Throwaway repo to exercise the `concurrency:` block from git/git PR #2369
("ci: cancel stale pull request workflow runs"). The workflow does nothing but
echo its concurrency group and sleep, so the run lifecycle (cancelled vs
completed vs queued) is what we observe.
