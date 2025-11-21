#!/usr/bin/env bb

(ns jank.compiler+runtime.bash-test
  (:require [clojure.string]
            [clojure.core]
            [jank.util :as util]
            [babashka.process :as b.p]
            [babashka.fs :as b.f]))

(def compiler+runtime-dir (str (b.f/parent *file*) "/../../.."))

(defn run-test [bash-test-dir extra-env test-file]
  (let [skip? (clojure.string/ends-with? (str test-file) "skip-test")
        expect-failure? (clojure.string/ends-with? (str test-file) "fail-test")
        dirname (b.f/parent test-file)
        relative-dirname (b.f/relativize bash-test-dir dirname)]
    (if skip?
      {:type :skip :path relative-dirname}
      (let [start-time (System/nanoTime)
            res @(b.p/process {:out :string
                               :err :out
                               :dir dirname
                               :extra-env extra-env}
                              test-file)
            duration (long (/ (double (- (System/nanoTime) start-time)) 1000000.0))
            unexpected? (or (and (zero? (:exit res)) expect-failure?)
                            (and (not (zero? (:exit res))) (not expect-failure?)))]
        (if unexpected?
          {:type :failed :path relative-dirname :result res :output (:out res) :duration duration}
          {:type :passed :path relative-dirname :duration duration})))))

(defn -main [{:keys [enabled?]}]
  (util/log-step "Run bash test suite")
  (if-not enabled?
    (util/log-info "Not enabled")
    (let [bash-test-dir (str compiler+runtime-dir "/test/bash")
          test-files (vec (b.f/glob bash-test-dir "**/{pass,fail,skip}-test"))
          extra-env (merge {"PATH" (str compiler+runtime-dir "/build" ":" (util/get-env "PATH"))}
                           (let [skip (System/getenv "JANK_SKIP_AOT_CHECK")]
                             (when-not (empty? skip)
                               {"JANK_SKIP_AOT_CHECK" skip})))
          test-paths (vec (map #(b.f/relativize bash-test-dir (b.f/parent %)) test-files))
          path-to-idx (zipmap test-paths (range))
          failed-results (atom [])
          completed (atom #{})]
      ;; Print all pending tests first
      (doseq [path test-paths]
        (println (str "🛈  Pending testing " path " [.]")))
      (flush)
      
      ;; Start animation thread
      (let [animation-thread (future
                               (loop [frame 0]
                                 (when (< (count @completed) (count test-paths))
                                   (Thread/sleep 500)
                                   (let [spinner ["[.]" "[..]" "[...]"]
                                         current-frame (mod frame 3)
                                         spinner-text (nth spinner current-frame)]
                                     (doseq [path test-paths]
                                       (when-not (contains? @completed path)
                                         (let [idx (get path-to-idx path)
                                               lines-to-move (- (count test-paths) idx 1)]
                                           (when (pos? lines-to-move)
                                             (print (str "\u001b[" lines-to-move "A")))
                                           (print "\u001b[2K\r")
                                           (print (str "🛈  Pending testing " path " " spinner-text))
                                           (when (pos? lines-to-move)
                                             (print (str "\u001b[" lines-to-move "B")))
                                           (flush))))
                                   (recur (inc frame))))))]
        
        ;; Now run tests in parallel and update as they complete
        (doall
          (pmap (fn [test-file]
                  (let [result (run-test bash-test-dir extra-env test-file)
                        path (:path result)
                        idx (get path-to-idx path)
                        lines-to-move (- (count test-paths) idx 1)]
                    (case (:type result)
                      :skip (do
                              (swap! completed conj path)
                              (util/log-warning "Skipped " path))
                      :passed (do
                                (swap! completed conj path)
                                ;; Move up to the pending line for this test
                                (when (pos? lines-to-move)
                                  (print (str "\u001b[" lines-to-move "A")))
                                (print "\u001b[2K\r")  ;; Clear line and carriage return
                                (print (str "🛈  Done testing " path " (" (util/format-ms (:duration result)) ")"))
                                ;; Move back down if we moved up
                                (when (pos? lines-to-move)
                                  (print (str "\u001b[" lines-to-move "B")))
                                (flush))
                      :failed (do
                                (swap! completed conj path)
                                (util/log-error-with-time (:duration result) "Failed with exit code " (:exit (:result result)))
                                (println (:output result))))
                    result))
                test-files))
        
        ;; Wait for animation thread to finish
        @animation-thread)
      
      (println)
      (when (seq @failed-results)
        (System/exit 1)))))

(when (= *file* (System/getProperty "babashka.file"))
  (-main {:enabled? true}))
