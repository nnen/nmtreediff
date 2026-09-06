Code Guidelines
===============

* Document all code using Doxygen documentation comments. Use MSDN API
  reference style for the documentation comments.
* Inside function implementations, put comments explaining things that aren't
  obvious. For each logical *block* of code, put a single-line summary of what
  that block does to help readers navigate through functions.
* Avoid magic constants. Use named constants instead and re-use them where it
  makes sense.
* Avoid functions that are too long. Break them into smaller functions.
  Particularly, if a big part of a function is a loop iterating over elements
  in some collection, extract the body of that loop into a separate function
  that handles single element.

