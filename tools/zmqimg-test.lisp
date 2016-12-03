;; (mapc #'ql:quickload '("pzmq" "static-vectors" "trivial-garbage"))

(defpackage #:zmqimg-test
  (:use #:cl)
  (:import-from #:static-vectors
                #:with-static-vector
                #:replace-foreign-memory
                #:static-vector-pointer
                #:free-static-vector)
  (:export
   #:serve
   #:image
   #:planes
   #:meta
   #:plane
   #:width
   #:height
   #:max-step
   #:line-size
   #:data))

(in-package #:zmqimg-test)

(defclass image ()
  ((planes :initarg :planes :reader planes)
   (meta :initarg :meta :reader meta)))

(defclass plane ()
  ((width :initarg :width :reader width)
   (height :initarg :height :reader height)
   (max-step :initarg :max-step :reader max-step)
   (line-size :initarg :line-size :reader line-size)
   (data :initarg :data :reader data)))

(defun serve (&optional (processor #'identity))
  (pzmq:with-socket s :rep
    (pzmq:bind s "tcp://*:5556")
    (loop
     (let ((image (receive-image s)))
       (send-image (funcall processor image) s)))))

(defun receive-message-parts (s)
  ;; FIXME: The static vectors will be exposed to memory leaks until
  ;; later on, when we add a finalizer to free them.
  (let* ((parts '())
         (meta (receive-meta s)))
    (loop
     ;; It seems we cannot use zmq_msg_init_data because zmq "takes
     ;; ownership" of the buffer; tried that and the data was
     ;; corrupted.  Oh well, copy.
     (pzmq:with-message msg
       (pzmq:msg-recv msg s)
       (with-static-vector (sv (pzmq:msg-size msg))
         (replace-foreign-memory
          (static-vector-pointer sv)
          (pzmq:msg-data msg)
          (pzmq:msg-size msg))
         (push sv parts)
         (setf sv nil))
       (when (not (pzmq:getsockopt s :rcvmore))
         (return))))
    (assert (= (length meta) (length parts)))
    (values meta (nreverse parts))))

(defun receive-meta (s)
  (multiple-value-bind (str more) (pzmq:recv-string s)
    (when (not more)
      (error "No planes?"))
    (read-from-string str)))

(defun receive-image (s)
  (multiple-value-bind (meta svs) (receive-message-parts s)
    (make-instance 'image
                   :meta meta
                   :planes (mapcar #'make-plane meta svs))))

(defun make-plane (pmeta sv)
  (destructuring-bind (w h ms ls) pmeta
    (tg:finalize
     (make-instance 'plane
                    :width w
                    :height h
                    :max-step ms
                    :line-size ls
                    :data sv)
     (lambda ()
       (free-static-vector sv)))))

(defun send-image (image s)
  (do ((planes (planes image) (rest planes)))
      ((null planes))
    (let ((data (data (first planes)))
          (more (not (null (rest planes)))))
      (pzmq:send s
                 (static-vector-pointer data)
                 :len (length data)
                 :sndmore more))))
