# Helping out

There are plenty of useful ways to help CYGM without writing firmware. Testing a new build, documenting a repeatable bug, or confirming a fix on a different glucose provider all count.

## Found a bug?

Search the [issues](https://github.com/decibelkaos/CYGM/issues) first. If it's new, open one with the firmware version, your board and power setup, your CGM provider, and clear steps to reproduce the problem.

Don't attach API tokens, passwords, or personally identifiable glucose records.

## Have a fix?

Fork the repository and open a focused pull request. Explain the behavior you changed and how you tested it. Please say when something has only been compiled, not run on hardware. Contributors and maintainers can use the repository's [contribution guide](../../CONTRIBUTING.md) for more detail once it is merged.

A proposed fix is not automatically included in a beta or stable release. Changes need review and device testing first.

## Community contributors

- [@monizks-prog](https://github.com/monizks-prog): investigated USB display dimming ([issue #3](https://github.com/decibelkaos/CYGM/issues/3), [PR #4](https://github.com/decibelkaos/CYGM/pull/4)) and duplicate-reading age behavior ([issue #5](https://github.com/decibelkaos/CYGM/issues/5)). Fixes are pending maintainer verification.
- [@steveR-bit](https://github.com/steveR-bit): reported and verified the UK LibreLinkUp login fix ([issue #2](https://github.com/decibelkaos/CYGM/issues/2)).
- [@SimonXuku](https://github.com/SimonXuku): reported and verified the fix for settings after power loss ([issue #1](https://github.com/decibelkaos/CYGM/issues/1)).

Thanks for putting real time into testing and reporting issues. It helps catch problems that don't always show up on the development bench.
