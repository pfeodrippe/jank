;;; ios-eval.el --- Simple iOS eval integration for jank -*- lexical-binding: t -*-

;; Author: jank iOS integration
;; Version: 0.1
;; Keywords: jank, clojure, ios, repl

;;; Commentary:

;; This provides simple REPL-like interaction with jank running on iOS.
;; It follows the ClojureScript model: all IDE features (completion, lookup)
;; stay on macOS, only eval is forwarded to iOS.
;;
;; Usage:
;;   1. Start jank on iOS device (with eval server on port 5558)
;;   2. M-x ios-eval-connect RET <ios-device-ip> RET
;;   3. Use C-c C-i to eval sexp at point
;;   4. Use C-c C-r to eval region
;;   5. Use C-c C-b to eval buffer

;;; Code:

(require 'json)

(defgroup ios-eval nil
  "iOS eval integration for jank."
  :prefix "ios-eval-"
  :group 'languages)

(defcustom ios-eval-host nil
  "iOS device IP address."
  :type 'string
  :group 'ios-eval)

(defcustom ios-eval-port 5558
  "iOS eval server port."
  :type 'integer
  :group 'ios-eval)

(defcustom ios-eval-timeout 30
  "Timeout in seconds for eval requests."
  :type 'integer
  :group 'ios-eval)

(defvar ios-eval--process nil
  "Network process for iOS eval connection.")

(defvar ios-eval--response nil
  "Buffer for accumulating response.")

(defvar ios-eval--callback nil
  "Callback for current request.")

(defvar ios-eval--request-id 0
  "Counter for request IDs.")

(defun ios-eval--filter (proc string)
  "Filter function for network process PROC receiving STRING."
  (setq ios-eval--response (concat ios-eval--response string))
  ;; Check for complete response (ends with newline)
  (when (string-match "\n$" ios-eval--response)
    (let ((response ios-eval--response))
      (setq ios-eval--response nil)
      (when ios-eval--callback
        (funcall ios-eval--callback response)
        (setq ios-eval--callback nil)))))

(defun ios-eval--sentinel (proc event)
  "Sentinel function for network process PROC with EVENT."
  (when (string-match "\\(closed\\|deleted\\|failed\\)" event)
    (message "[ios-eval] Disconnected: %s" (string-trim event))
    (setq ios-eval--process nil)))

;;;###autoload
(defun ios-eval-connect (host &optional port)
  "Connect to iOS eval server at HOST:PORT."
  (interactive
   (list (read-string "iOS device IP: " ios-eval-host)
         (read-number "Port: " ios-eval-port)))
  (when ios-eval--process
    (delete-process ios-eval--process))
  (setq ios-eval-host host)
  (setq ios-eval-port (or port ios-eval-port))
  (condition-case err
      (progn
        (setq ios-eval--process
              (make-network-process
               :name "ios-eval"
               :host host
               :service ios-eval-port
               :filter #'ios-eval--filter
               :sentinel #'ios-eval--sentinel
               :coding 'utf-8))
        (setq ios-eval--response nil)
        ;; Wait for welcome message
        (sleep-for 0.2)
        (message "[ios-eval] Connected to %s:%d" host ios-eval-port))
    (error
     (message "[ios-eval] Failed to connect: %s" (error-message-string err)))))

;;;###autoload
(defun ios-eval-disconnect ()
  "Disconnect from iOS eval server."
  (interactive)
  (when ios-eval--process
    (delete-process ios-eval--process)
    (setq ios-eval--process nil)
    (message "[ios-eval] Disconnected")))

(defun ios-eval--send (code callback)
  "Send CODE for evaluation, call CALLBACK with result."
  (unless ios-eval--process
    (error "Not connected to iOS. Use M-x ios-eval-connect"))
  (setq ios-eval--request-id (1+ ios-eval--request-id))
  (setq ios-eval--callback callback)
  (setq ios-eval--response nil)
  (let* ((request (json-encode
                   `((op . "eval")
                     (id . ,ios-eval--request-id)
                     (code . ,code)
                     (ns . "user")))))
    (process-send-string ios-eval--process (concat request "\n"))))

(defun ios-eval--handle-response (response)
  "Handle RESPONSE from iOS eval server."
  (condition-case err
      (let* ((json-object-type 'alist)
             (parsed (json-read-from-string response))
             (op (alist-get 'op parsed))
             (value (alist-get 'value parsed))
             (error-msg (alist-get 'error parsed)))
        (cond
         ((string= op "result")
          (message "=> %s" value)
          (with-current-buffer (get-buffer-create "*ios-eval*")
            (goto-char (point-max))
            (insert (format "\n=> %s" value))))
         ((string= op "error")
          (message "[ios-eval] Error: %s" error-msg)
          (with-current-buffer (get-buffer-create "*ios-eval*")
            (goto-char (point-max))
            (insert (format "\n;; Error: %s" error-msg))))
         (t
          (message "[ios-eval] Unknown response: %s" response))))
    (error
     (message "[ios-eval] Failed to parse response: %s" (error-message-string err)))))

;;;###autoload
(defun ios-eval-string (code)
  "Evaluate CODE on iOS device."
  (interactive "sEval: ")
  (ios-eval--send code #'ios-eval--handle-response))

;;;###autoload
(defun ios-eval-last-sexp ()
  "Evaluate sexp before point on iOS device."
  (interactive)
  (let ((end (point))
        (start (save-excursion
                 (backward-sexp)
                 (point))))
    (ios-eval-string (buffer-substring-no-properties start end))))

;;;###autoload
(defun ios-eval-defun ()
  "Evaluate defun at point on iOS device."
  (interactive)
  (save-excursion
    (end-of-defun)
    (let ((end (point)))
      (beginning-of-defun)
      (ios-eval-string (buffer-substring-no-properties (point) end)))))

;;;###autoload
(defun ios-eval-region (start end)
  "Evaluate region from START to END on iOS device."
  (interactive "r")
  (ios-eval-string (buffer-substring-no-properties start end)))

;;;###autoload
(defun ios-eval-buffer ()
  "Evaluate current buffer on iOS device."
  (interactive)
  (ios-eval-string (buffer-substring-no-properties (point-min) (point-max))))

;;;###autoload
(defun ios-eval-ping ()
  "Ping iOS eval server."
  (interactive)
  (unless ios-eval--process
    (error "Not connected to iOS. Use M-x ios-eval-connect"))
  (setq ios-eval--request-id (1+ ios-eval--request-id))
  (setq ios-eval--callback
        (lambda (response)
          (if (string-match "pong" response)
              (message "[ios-eval] Pong!")
            (message "[ios-eval] Unexpected response: %s" response))))
  (setq ios-eval--response nil)
  (process-send-string ios-eval--process
                       (format "{\"op\":\"ping\",\"id\":%d}\n"
                               ios-eval--request-id)))

;; Keybindings
(defvar ios-eval-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map (kbd "C-c C-i") #'ios-eval-last-sexp)
    (define-key map (kbd "C-c C-d") #'ios-eval-defun)
    (define-key map (kbd "C-c C-r") #'ios-eval-region)
    (define-key map (kbd "C-c C-b") #'ios-eval-buffer)
    map)
  "Keymap for ios-eval-mode.")

;;;###autoload
(define-minor-mode ios-eval-mode
  "Minor mode for iOS eval integration.
\\{ios-eval-mode-map}"
  :lighter " iOS"
  :keymap ios-eval-mode-map)

(provide 'ios-eval)
;;; ios-eval.el ends here
