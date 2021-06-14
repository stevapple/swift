# GSoC 2021: SwiftPM Support for Swift Packages

## Phase 1: Teaching the frontend to recognize package declaration syntax

### Determined tasks

- [x] The `@package` attribute;
- [x] The `ignore-package-declaration` flag;
- [x] Basic syntax checking for `@package`;
- [ ] The `emit-package-declaration` flag and output format;
- [ ] Teach the parser to read labels from external syntax file;
- [ ] Teach the type checker to check against external syntax file.

### Future tasks

- [ ] Integrated driver support (probably move to the second phase?);
- [ ] Test SourceKit;
- [ ] Write unit tests.

### Needs further investigation

- How to import the syntax from SwiftPM?
- How to generate the syntax definition file?
- How to typecheck the attribute in an "isolated" context?

## Phase 2: Integrating the feature into the driver and package manager

TBD.

## Notes from the discussions

It seems best to provide package declaration syntax with a specific file from
SwiftPM, and we can expose it in some way that IDEs can also use.
