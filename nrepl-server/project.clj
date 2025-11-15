(defproject org.jank-lang/nrepl-server "0.1.0-SNAPSHOT"
  :license {:name "MPL 2.0"
            :url "https://www.mozilla.org/en-US/MPL/2.0/"}
  :dependencies [[org.clojure/clojure "1.11.1"]]
  :plugins [[org.jank-lang/lein-jank "0.2"]]
  :main ^:skip-aot jank.nrepl-server.core
  :target-path "target/%s"
  :jank {:include-paths []
         :include-dirs ["/opt/homebrew/include"]
         :library-dirs ["/opt/homebrew/lib"
                        "/usr/lib"]}
  :source-paths ["src/jank"
                 "src/cpp"]
  :profiles {:uberjar {:aot :all
                       :jvm-opts ["-Dclojure.compiler.direct-linking=true"]}})
