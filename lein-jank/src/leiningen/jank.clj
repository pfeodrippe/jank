(ns leiningen.jank
  (:refer-clojure :exclude [run!])
  (:require [babashka.process :as ps]
            [babashka.fs :as b.f]
            [clojure.java.io :as io]
            [clojure.pprint :as pp]
            [clojure.string :as string]
            [leiningen.core.classpath :as lcp]
            [leiningen.core.main :as lmain])
  (:import [java.io File]))

(defonce verbose? (atom false))

(defn build-declarative-flag [flag value]
  (case flag
    :direct-call
    (when value
      "--direct-call")

    :optimization-level
    ; TODO: Validate.
    (str "-O" value)

    :codegen
    (str "--codegen " (name value))

    :defines
    (map (fn [[k v]] (str "-D" k "=" v)) value)

    :include-dirs
    (map (fn [v] (str "-I" v)) value)

    :include-paths
    (map (fn [v] (str "-I" v)) value)

    :library-dirs
    (map (fn [v] (str "-L" v)) value)

    :linked-libraries
    (map (fn [v] (str "-l" v)) value)

    (lmain/warn (str "Unknown flag " flag))))

(defn build-declarative-flags [project]
  (->> (:jank project)
       (mapcat (fn [[flag value]]
                 (let [result (build-declarative-flag flag value)]
                   (cond
                     (nil? result) []
                     (string? result) [result]
                     (sequential? result) result
                     :else [result]))))))

<<<<<<< HEAD

(defn- absolutize-if-file [path]
  (when path
    (let [p (b.f/path path)]
      (when (and (b.f/exists? p)
                 (not (b.f/directory? p)))
        (str (b.f/absolutize p))))))

(defn- existing-dir [path]
  (when path
    (let [f (io/file path)]
      (when (.isDirectory f)
        (.getAbsolutePath (.getCanonicalFile f))))))

(defn- discover-include-paths []
  (->> [(System/getenv "BOOST_INCLUDEDIR")
        (some-> (System/getenv "HOMEBREW_PREFIX") (str "/include"))
        "/opt/homebrew/include"
        "/usr/local/include"
        "/usr/include"]
       (map existing-dir)
       (remove nil?)
       distinct
       vec))

(defn- windows? []
  (.startsWith (System/getProperty "os.name") "Windows"))

(defn- collect-parents [path]
  (loop [current path
         acc []]
    (if current
      (recur (b.f/parent current)
             (conj acc current))
      acc)))

(defn- discover-jank-executable [project]
  (let [env-jank (some-> (System/getenv "JANK_BIN") not-empty)
        configured-jank (:jank-bin project)
        os-exec (if (windows?) "jank.exe" "jank")
        project-root (some-> (:root project) b.f/path b.f/absolutize)
        parent-roots (if project-root
                       (collect-parents project-root)
                       [])
        repo-local-paths (map #(b.f/path % "compiler+runtime" "build" os-exec)
                               parent-roots)
        candidates (concat [(b.f/which "jank") env-jank configured-jank]
                            repo-local-paths)]
    (some absolutize-if-file candidates)))

=======
>>>>>>> origin/main
(defn shell-out! [project classpath command compiler-args runtime-args]
  (let [jank (discover-jank-executable project)
        ; TODO: Better error handling.
<<<<<<< HEAD
        _ (when-not jank
            (lmain/warn "Unable to locate the `jank` executable. Did you run ./bin/compile under compiler+runtime, or add build/jank to your PATH?")
            (lmain/warn "Set JANK_BIN or :jank-bin in project.clj if you need a custom location.")
            (lmain/exit 1))
          include-paths (seq (get-in project [:jank :include-paths]))
          inferred-includes (when-not include-paths (discover-include-paths))
          project (if (and (not include-paths) (seq inferred-includes))
                    (assoc-in project [:jank :include-paths] inferred-includes)
                    project)
          env (System/getenv)
          compiler-args (map str compiler-args)
          runtime-args (map str runtime-args)
          flags (vec (build-declarative-flags project))
          raw-args (concat [jank "--module-path" classpath]
                            flags
                            [command]
                            compiler-args
                            (if (seq runtime-args)
                              (concat ["--"] runtime-args)
                              []))
          args (->> raw-args
                     (map str)
                     vec)
=======
        _ (assert (some? jank))
        _ (when @verbose?
            (println ">" (clojure.string/join " " args)))
>>>>>>> origin/main
        proc (apply ps/shell
                    {:continue true
                     :dir (:root project)
                     :extra-env env}
                    args)]
    (when-not (zero? (:exit proc))
      (System/exit (:exit proc)))))

(defn build-module-path [project]
  (->> project
       lcp/get-classpath
       (string/join File/pathSeparatorChar)))

(defn run!
  "Run your project, starting at the main entrypoint."
  [project & args]
  (let [cp-str (build-module-path project)]
    (if (:main project)
      (shell-out! project cp-str "run-main" [(:main project)] args)
      (do
        (lmain/warn "No :main entrypoint for project.")
        (lmain/exit 1)))))

(defn repl!
  "Start a terminal REPL in your :main ns."
  [project & args]
  (let [cp-str (build-module-path project)]
    (if (:main project)
      (shell-out! project cp-str "repl" [(:main project)] args)
      (do
        (lmain/warn "No :main entrypoint for project.")
        (lmain/exit 1)))))

(defn compile!
  "Compile your project to an executable."
  [project & args]
  (let [cp-str (build-module-path project)]
    (if (:main project)
      (shell-out! project cp-str "compile" [(:main project)] args)
      (do
        (lmain/warn "No :main entrypoint for project.")
        (lmain/exit 1)))))

(defn compile-module!
  "Compile a single module and its dependencies to object files."
  [project & args]
  (let [cp-str (build-module-path project)]
    (shell-out! project cp-str "compile-module" [] args)))

(defn check-health!
  "Perform a health check on your jank install."
  [project & args]
  (let [cp-str (build-module-path project)]
    (shell-out! project cp-str "check-health" [] args)))

(declare print-help!)

(def subtask-kw->var {:run #'run!
                      :repl #'repl!
                      :compile #'compile!
                      :compile-module #'compile-module!
                      :check-health #'check-health!
                      :help #'print-help!})

(defn print-help!
  "Show this help message."
  [& _args]
  (pp/print-table
    (map (fn [[sub fn-ref]]
           {:sub-command (name sub)
            :help (-> fn-ref meta :doc)})
         subtask-kw->var)))

(defn process-args [args]
  (loop [args args
         ret []]
    (if (empty? args)
      ret
      (let [arg (first args)
            ret (case arg
                  "-v" (do
                         (reset! verbose? true)
                         ret)
                  (conj ret arg))]
        (recur (rest args) ret)))))

(defn jank
  "Compile, run and repl into jank."
  [project subcmd & args]
  ;(clojure.pprint/pprint (:jank project))
  (if-some [handler (subtask-kw->var (keyword subcmd))]
    (apply handler project (process-args args))
    (do
      (lmain/warn "Invalid subcommand!")
      (print-help!)
      (lmain/exit 1))))

(def default-project {:aliases {"run" ^{:doc "Run your project, starting at the main entrypoint."}
                                ["jank" "run"]

                                "repl" ^{:doc "Start a terminal REPL in your :main ns."}
                                ["jank" "repl"]

                                "compile" ^{:doc "Compile your project to an executable."}
                                ["jank" "compile"]

                                "compile-module" ^{:doc "Compile a single module and its dependencies to object files."}
                                ["jank" "compile-module"]

                                "test" ^{:doc "Run your project's test suite."}
                                ["jank" "test"]

                                "check-health" ^{:doc "Perform a health check on your jank install."}
                                ["jank" "check-health"]}})

(defn deep-merge* [& maps]
  (let [f (fn [old new]
            (if (and (map? old) (map? new))
              (merge-with deep-merge* old new)
              new))]
    (if (every? map? maps)
      (apply merge-with f maps)
      (last maps))))

(defn deep-merge [& maps]
  (let [maps (filter some? maps)]
    (apply merge-with deep-merge* maps)))

(defn middleware
  "Inject jank project details into your current project."
  [project]
  (deep-merge default-project project))
