# Atoms
An atom is any value that is not a list.
## Numbers
### Integers
```
-> 42
42
```
### Floating-point numbers
```
-> 3.14 
3.14
```
### Complex numbers
```
-> 1.1+2.2i
1.1+2.2i
```
## Symbols
```
-> 'foo
foo
```
## Strings
- Strings are immutable.
```
-> "bar"
bar
```
# Lists
# Callables
## subr
## nsubr
## lambda
## lambda*
## nlambda
# Environment
- The environment is a stack of dictionaries.
- A symbol lookup begins with the dictionary at the top of the environment stack, and searches up, until
  * the symbol is found, or
  * the top-most (i.e. main) environment is reached, and the symbol is not found.
- Symbol lookup ends at a module's environment
- An enviroment is created, and entered, by the _let_ and _let*_ functions
- A module is created by the _load_ function
- An environment can be enter by calling the _enter_ function
