{-|
Module      : Z.IO.Exception
Description : Extensible IO exceptions
Copyright   : (c) Dong Han, 2017-2018
License     : BSD
Maintainer  : winterland1989@gmail.com
Stability   : experimental
Portability : non-portable

This module implemented extensible io exception following approach described in /An Extensible Dynamically-Typed
Hierarchy of Exceptions/ by Simon Marlow. The implementation in this module has simplified to meet common need.
User who want to catch certain type of exceptions can directly use exception types this module provide,
which are modeled after @IOErrorType@ from "GHC.IO.Exception".


Functions from this package will throw exceptions from this module only instead of the old 'IOError' on IO exceptions.
Exceptions from this module contain 'IOEInfo' which is pretty detailed, but this also require user of this module
do some extra work to keep error message's quality(provide CallStack, device informations, etc.).
New defined IO exceptions are encouraged to include a 'IOEInfo', since it helps a lot when debugging.

Example for library author defining new io exception:

@
  data MyNetworkException = MyNetworkException IOEInfo ... deriving Show
  instance Exception MyNetworkException where
        toException = ioExceptionToException
        fromException = ioExceptionFromException
@

If you're dealing with OS's errno directly, you should convert the errno to libuv's errno in C side with
'uv_translate_sys_error' from @hs_uv.h@, then use 'throwUVIfMinus\/throwUVError' from this module.

-}

module Z.IO.Exception
  ( -- * The 'SomeIOException' type
    SomeIOException(..)
  , ioExceptionToException
  , ioExceptionFromException
    -- * Builtin io exception types
  , IOEInfo(..)
  , AlreadyExists(..)
  , NoSuchThing(..)
  , ResourceBusy(..)
  , ResourceExhausted(..)
  , EOF(..)
  , IllegalOperation(..)
  , PermissionDenied(..)
  , UnsatisfiedConstraints(..)
  , SystemError(..)
  , ProtocolError(..)
  , OtherError(..)
  , InvalidArgument(..)
  , InappropriateType(..)
  , HardwareFault(..)
  , UnsupportedOperation(..)
  , TimeExpired(..)
  , ResourceVanished(..)
  , Interrupted(..)
    -- * Throw io exceptions
  , throwOOMIfNull
  , throwUVIfMinus
  , throwUVIfMinus_
  , throwECLOSED
  , throwECLOSEDSTM
  , throwUVError
    -- * Re-exports
  , module Control.Exception
  , HasCallStack
  , CallStack
  , callStack
  , module Z.IO.UV.Errno
  ) where

import           Control.Concurrent.STM
import           Control.Exception      hiding (IOException)
import           Control.Monad
import           Data.Typeable          (cast)
import           Foreign.C.Types
import           Foreign.Ptr
import           GHC.Stack
import qualified Z.Data.Text            as T
import qualified Z.Data.Text.ShowT      as T
import           Z.IO.UV.Errno


-- | The root type of all io exceptions, you can catch all io exception by catching this root type.
--
data SomeIOException = forall e . Exception e => SomeIOException e

instance Show SomeIOException where
    show (SomeIOException e) = show e

instance Exception SomeIOException

ioExceptionToException :: Exception e => e -> SomeException
ioExceptionToException = toException . SomeIOException

ioExceptionFromException :: Exception e => SomeException -> Maybe e
ioExceptionFromException x = do
    SomeIOException a <- fromException x
    cast a

#define IOE(e) data e = e IOEInfo deriving (Show);  \
               instance Exception e where                     \
                   { toException = ioExceptionToException     \
                   ; fromException = ioExceptionFromException \
                   }
IOE(AlreadyExists)
IOE(NoSuchThing)
IOE(ResourceBusy)
IOE(ResourceExhausted)
IOE(EOF)
IOE(IllegalOperation)
IOE(PermissionDenied)
IOE(UnsatisfiedConstraints)
IOE(SystemError)
IOE(ProtocolError)
IOE(OtherError)
IOE(InvalidArgument)
IOE(InappropriateType)
IOE(HardwareFault)
IOE(UnsupportedOperation)
IOE(TimeExpired)
IOE(ResourceVanished)
IOE(Interrupted)

--------------------------------------------------------------------------------

-- | Throw 'ResourceExhausted' if allocation return a 'nullPtr'.
--
throwOOMIfNull :: HasCallStack
               => IO (Ptr a)    -- ^ the allocation action
               -> IO (Ptr a)
{-# INLINABLE throwOOMIfNull #-}
throwOOMIfNull f = do
    addr <- f
    if addr == nullPtr
        then throwIO (ResourceExhausted (IOEInfo "OOM" "out of memory when doing allocation" callStack))
        else return addr

-- | Throw appropriate IO exception if return value < 0 (libuv's convention).
--
throwUVIfMinus :: (HasCallStack, Integral a)
               => IO a    -- ^ the IO action
               -> IO a
{-# INLINABLE throwUVIfMinus #-}
throwUVIfMinus f = do
    errno <- f
    let errno' = fromIntegral errno
    if errno' < 0
        then do
            name <- uvErrName errno'
            desc <- uvStdError errno'
            throwUVError errno' (IOEInfo name desc callStack)
        else return errno

-- | Throw appropriate IO exception if return value < 0, otherwise ignore the result.
--
throwUVIfMinus_ :: (HasCallStack, Integral a)
                => IO a    -- ^ the IO action
                -> IO ()
{-# INLINABLE throwUVIfMinus_ #-}
throwUVIfMinus_ f = do
    errno <- f
    let errno' = fromIntegral errno
    when (errno' < 0) $ do
        name <- uvErrName errno'
        desc <- uvStdError errno'
        throwUVError errno' (IOEInfo name desc callStack)

-- | Throw 'E.ResourceVanished' with name 'ECLOSED' and description 'resource is closed'.
--
throwECLOSED :: HasCallStack => IO a
throwECLOSED = throwIO (ResourceVanished
    (IOEInfo "ECLOSED" "resource is closed" callStack))

throwECLOSEDSTM :: HasCallStack => STM a
throwECLOSEDSTM = throwSTM (ResourceVanished
    (IOEInfo "ECLOSED" "resource is closed" callStack))

--------------------------------------------------------------------------------

-- | IO exceptions informations.
--
data IOEInfo = IOEInfo
    { ioeName        :: T.Text      -- ^ the errno name, e.g. EADDRINUSE, etc. empty if no errno.
    , ioeDescription :: T.Text      -- ^ description for this io error, can be errno description, or some custom description if no errno.
    , ioeCallStack   :: CallStack   -- ^ lightweight partial call-stack
    }

instance Show IOEInfo where show = T.toString

instance T.ShowT IOEInfo where
    toUTF8BuilderP _ (IOEInfo errno desc cstack) = do
         "{name:"
         T.text errno
         ", description:"
         T.text desc
         ", callstack:"
         T.stringUTF8 (prettyCallStack cstack)
         "}"

throwUVError :: CInt -> IOEInfo -> IO a
throwUVError e info = case e of
    UV_EOF             -> throwIO (EOF                     info)
    UV_E2BIG           -> throwIO (ResourceExhausted       info)
    UV_EACCES          -> throwIO (PermissionDenied        info)
    UV_EADDRINUSE      -> throwIO (ResourceBusy            info)
    UV_EADDRNOTAVAIL   -> throwIO (UnsupportedOperation    info)
    UV_EAFNOSUPPORT    -> throwIO (UnsupportedOperation    info)
    UV_EAGAIN          -> throwIO (ResourceExhausted       info)
    UV_EAI_ADDRFAMILY  -> throwIO (UnsupportedOperation    info)
    UV_EAI_AGAIN       -> throwIO (ResourceExhausted       info)
    UV_EAI_BADFLAGS    -> throwIO (UnsupportedOperation    info)
    UV_EAI_BADHINTS    -> throwIO (UnsupportedOperation    info)
    UV_EAI_CANCELED    -> throwIO (ResourceVanished        info)
    UV_EAI_FAIL        -> throwIO (OtherError              info)
    UV_EAI_FAMILY      -> throwIO (UnsupportedOperation    info)
    UV_EAI_MEMORY      -> throwIO (ResourceExhausted       info)
    UV_EAI_NODATA      -> throwIO (NoSuchThing             info)
    UV_EAI_NONAME      -> throwIO (NoSuchThing             info)
    UV_EAI_OVERFLOW    -> throwIO (InvalidArgument         info)
    UV_EAI_PROTOCOL    -> throwIO (ProtocolError           info)
    UV_EAI_SERVICE     -> throwIO (UnsupportedOperation    info)
    UV_EAI_SOCKTYPE    -> throwIO (UnsupportedOperation    info)
    UV_EALREADY        -> throwIO (AlreadyExists           info)
    UV_EBADF           -> throwIO (InvalidArgument         info)
    UV_EBUSY           -> throwIO (ResourceBusy            info)
    UV_ECANCELED       -> throwIO (ResourceVanished        info)
    UV_ECHARSET        -> throwIO (OtherError              info)
    UV_ECONNABORTED    -> throwIO (ResourceVanished        info)
    UV_ECONNREFUSED    -> throwIO (NoSuchThing             info)
    UV_ECONNRESET      -> throwIO (ResourceVanished        info)
    UV_EDESTADDRREQ    -> throwIO (InvalidArgument         info)
    UV_EEXIST          -> throwIO (AlreadyExists           info)
    UV_EFAULT          -> throwIO (OtherError              info)
    UV_EFBIG           -> throwIO (PermissionDenied        info)
    UV_EHOSTUNREACH    -> throwIO (NoSuchThing             info)
    UV_EINTR           -> throwIO (Interrupted             info)
    UV_EINVAL          -> throwIO (InvalidArgument         info)
    UV_EIO             -> throwIO (HardwareFault           info)
    UV_EISCONN         -> throwIO (AlreadyExists           info)
    UV_EISDIR          -> throwIO (InappropriateType       info)
    UV_ELOOP           -> throwIO (InvalidArgument         info)
    UV_EMFILE          -> throwIO (ResourceExhausted       info)
    UV_EMSGSIZE        -> throwIO (InvalidArgument         info)
    UV_ENAMETOOLONG    -> throwIO (InvalidArgument         info)
    UV_ENETDOWN        -> throwIO (ResourceVanished        info)
    UV_ENETUNREACH     -> throwIO (NoSuchThing             info)
    UV_ENFILE          -> throwIO (ResourceExhausted       info)
    UV_ENOBUFS         -> throwIO (ResourceExhausted       info)
    UV_ENODEV          -> throwIO (UnsupportedOperation    info)
    UV_ENOENT          -> throwIO (NoSuchThing             info)
    UV_ENOMEM          -> throwIO (ResourceExhausted       info)
    UV_ENOPROTOOPT     -> throwIO (UnsupportedOperation    info)
    UV_ENOSPC          -> throwIO (ResourceExhausted       info)
    UV_ENOSYS          -> throwIO (UnsupportedOperation    info)
    UV_ENOTCONN        -> throwIO (InvalidArgument         info)
    UV_ENOTDIR         -> throwIO (InappropriateType       info)
    UV_ENOTEMPTY       -> throwIO (UnsatisfiedConstraints  info)
    UV_ENOTSOCK        -> throwIO (InvalidArgument         info)
    UV_ENOTSUP         -> throwIO (UnsupportedOperation    info)
    UV_EPERM           -> throwIO (PermissionDenied        info)
    UV_EPIPE           -> throwIO (ResourceVanished        info)
    UV_EPROTO          -> throwIO (ProtocolError           info)
    UV_EPROTONOSUPPORT -> throwIO (ProtocolError           info)
    UV_EPROTOTYPE      -> throwIO (ProtocolError           info)
    UV_ERANGE          -> throwIO (UnsupportedOperation    info)
    UV_EROFS           -> throwIO (PermissionDenied        info)
    UV_ESHUTDOWN       -> throwIO (IllegalOperation        info)
    UV_ESPIPE          -> throwIO (UnsupportedOperation    info)
    UV_ESRCH           -> throwIO (NoSuchThing             info)
    UV_ETIMEDOUT       -> throwIO (TimeExpired             info)
    UV_ETXTBSY         -> throwIO (ResourceBusy            info)
    UV_EXDEV           -> throwIO (UnsupportedOperation    info)
    UV_UNKNOWN         -> throwIO (OtherError              info)
    UV_ENXIO           -> throwIO (NoSuchThing             info)
    UV_EMLINK          -> throwIO (ResourceExhausted       info)
    _                  -> throwIO (OtherError              info)
