# jank Namespace Limitation: No Docstrings

**Important**: jank namespaces cannot have docstrings in the `ns` form.

## Wrong (will cause errors):
```clojure
(ns vybe.sdf.ios
  "iOS entry point - this docstring is NOT allowed!"
  (:require ...))
```

## Correct:
```clojure
(ns vybe.sdf.ios
  (:require ...))

;; iOS entry point - put comments outside the ns form
```

This differs from Clojure which allows docstrings in `ns` forms.
