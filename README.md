# LISP interpreter
## Introduction
I have always been fascinated by the LISP language, by its simplicity, and how from that simplicity grear power and elegance emerges.
### Goals
## Basic Concepts
### Experessions
#### Literals
##### Booleans
##### Integers
##### Floating-point Numbers
##### Complex Numbers
##### Strings
##### Symbols
#### Lists
### Evaluation
#### Atoms
#### Lists
#### Sequences
### Environment
### Callables
### Miscellaneous
## Examples
The setq function binds a value to a symbol in the current environment.  It is the equivalent of an assignment statement in other programming languages.
```
;; Bind the symbol foo to the value 42
-> (setq foo 42)
42
;; The symbol foo now evaluates to 42
-> foo
42
```
Unlike most other languages, setq is a function, like any other function, a;beit one with a side-effect of creating a binding, and so returns a value, and can be used in more complex expressions.  
The def function is much the same, except that the second ('value') argument is not evaluated.
```
;; Bind the symbol a to the symbol b
-> (def a b)
b
;; The symbol a now evaluates to the symbol b
-> a
b
;; Define a function, i.e. bind a lambda-expression to a symbol
-> (def bar (lambda (x) (add x x)))
(lambda (x) (add x x))
```
## How To Build
- Clone the repo: https://github.com/hpersh/ll6
- Cd to the "bin" directory
- Run the "make" command
- Executable is bin/ll
