# Security policy

metricsd ships inside the odi-oss image and is also released on its own, for
example to run on the stock firmware. Either way, a vulnerability in it is
reported through odi-oss.

## Supported versions

Supported are the latest metricsd release and the metricsd pinned by the latest
odi-oss release, on the odi-oss image or on its own. Older releases get no
patches. A fix ships as a new metricsd release and an odi-oss release that pins
it. If you run metricsd on its own, update to the new metricsd release. On the
odi-oss image, flash that odi-oss release.

## Reporting a vulnerability

Report it privately through GitHub private vulnerability reporting on
odi-oss: <https://github.com/AndreZiviani/odi-oss/security/advisories/new>

Please do not open a public issue or pull request for it. The odi-oss
[SECURITY.md](https://github.com/AndreZiviani/odi-oss/blob/main/SECURITY.md)
says what to include and how reports are answered.
