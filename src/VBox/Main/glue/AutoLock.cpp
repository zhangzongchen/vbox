/** @file
 *
 * Automatic locks, implementation
 */

/*
 * Copyright (C) 2006-2009 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

#include <iprt/cdefs.h>
#include <iprt/critsect.h>
#include <iprt/thread.h>
#include <iprt/semaphore.h>

#include <iprt/err.h>
#include <iprt/assert.h>

#if defined(DEBUG)
# include <iprt/asm.h> // for ASMReturnAddress
#endif

#include <iprt/string.h>
#include <iprt/path.h>
#include <iprt/stream.h>

#include "VBox/com/AutoLock.h"
#include <VBox/com/string.h>

#include <vector>
#include <list>
#include <map>

namespace util
{

////////////////////////////////////////////////////////////////////////////////
//
// RuntimeLockClass
//
////////////////////////////////////////////////////////////////////////////////

#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
typedef std::map<VBoxLockingClass, RTLOCKVALCLASS> LockValidationClassesMap;
LockValidationClassesMap g_mapLockValidationClasses;
#endif

/**
 * Called from initterm.cpp on process initialization (on the main thread)
 * to give us a chance to initialize lock validation runtime data.
 */
void InitAutoLockSystem()
{
#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    struct
    {
        VBoxLockingClass    cls;
        const char          *pcszDescription;
    } aClasses[] =
    {
        { LOCKCLASS_VIRTUALBOXOBJECT,   "1-VIRTUALBOXOBJECT" },
        { LOCKCLASS_USBPROXYSERVICE,    "2-USBPROXYSERVICE" },
        { LOCKCLASS_HOSTOBJECT,         "3-HOSTOBJECT" },
        { LOCKCLASS_LISTOFMACHINES,     "4-LISTOFMACHINES" },
        { LOCKCLASS_MACHINEOBJECT,      "5-MACHINEOBJECT" },
        { LOCKCLASS_LISTOFSNAPSHOTS,    "6-LISTOFSNAPSHOTS" },
        { LOCKCLASS_SNAPSHOTOBJECT,     "7-SNAPSHOTOBJECT" },
        { LOCKCLASS_LISTOFMEDIA,        "8-LISTOFMEDIA" },
        { LOCKCLASS_LISTOFOTHEROBJECTS, "9-LISTOFOTHEROBJECTS" },
        { LOCKCLASS_OTHEROBJECT,        "10-OTHEROBJECT" },
        { LOCKCLASS_USBLIST,            "11-USBLIST" },
        { LOCKCLASS_PROGRESSLIST,       "12-PROGRESSLIST" },
        { LOCKCLASS_OBJECTSTATE,        "13-OBJECTSTATE" }
    };

    RTLOCKVALCLASS hClass;
    int vrc;
    for (unsigned i = 0; i < RT_ELEMENTS(aClasses); ++i)
    {
        vrc = RTLockValidatorClassCreate(&hClass,
                                         true, /*fAutodidact*/
                                         RT_SRC_POS,
                                         aClasses[i].pcszDescription);
        AssertRC(vrc);

        // teach the new class that the classes created previously can be held
        // while the new class is being acquired
        for (LockValidationClassesMap::iterator it = g_mapLockValidationClasses.begin();
             it != g_mapLockValidationClasses.end();
             ++it)
        {
            RTLOCKVALCLASS &canBeHeld = it->second;
            vrc = RTLockValidatorClassAddPriorClass(hClass,
                                                    canBeHeld);
            AssertRC(vrc);
        }

        // and store the new class
        g_mapLockValidationClasses[aClasses[i].cls] = hClass;
    }

/*    WriteLockHandle critsect1(LOCKCLASS_VIRTUALBOXOBJECT);
    WriteLockHandle critsect2(LOCKCLASS_VIRTUALBOXLIST);

    AutoWriteLock lock1(critsect1 COMMA_LOCKVAL_SRC_POS);
    AutoWriteLock lock2(critsect2 COMMA_LOCKVAL_SRC_POS);*/
#endif
}

////////////////////////////////////////////////////////////////////////////////
//
// RWLockHandle
//
////////////////////////////////////////////////////////////////////////////////

struct RWLockHandle::Data
{
    Data()
    { }

    RTSEMRW                     sem;
    VBoxLockingClass   lockClass;

#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    com::Utf8Str                strDescription;
#endif
};

RWLockHandle::RWLockHandle(VBoxLockingClass lockClass)
{
    m = new Data();

    m->lockClass = lockClass;

#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    m->strDescription = com::Utf8StrFmt("r/w %RCv", this);
    int vrc = RTSemRWCreateEx(&m->sem, 0 /*fFlags*/, g_mapLockValidationClasses[lockClass], RTLOCKVAL_SUB_CLASS_ANY, NULL);
#else
    int vrc = RTSemRWCreateEx(&m->sem, 0 /*fFlags*/, NIL_RTLOCKVALCLASS, RTLOCKVAL_SUB_CLASS_ANY, NULL);
#endif
    AssertRC(vrc);
}

/*virtual*/ RWLockHandle::~RWLockHandle()
{
    RTSemRWDestroy(m->sem);
    delete m;
}

/*virtual*/ bool RWLockHandle::isWriteLockOnCurrentThread() const
{
    return RTSemRWIsWriteOwner(m->sem);
}

/*virtual*/ void RWLockHandle::lockWrite(LOCKVAL_SRC_POS_DECL)
{
#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    int vrc = RTSemRWRequestWriteDebug(m->sem, RT_INDEFINITE_WAIT, (uintptr_t)ASMReturnAddress(), RT_SRC_POS_ARGS);
#else
    int vrc = RTSemRWRequestWrite(m->sem, RT_INDEFINITE_WAIT);
#endif
    AssertRC(vrc);
}

/*virtual*/ void RWLockHandle::unlockWrite()
{
    int vrc = RTSemRWReleaseWrite(m->sem);
    AssertRC(vrc);

}

/*virtual*/ void RWLockHandle::lockRead(LOCKVAL_SRC_POS_DECL)
{
#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    int vrc = RTSemRWRequestReadDebug(m->sem, RT_INDEFINITE_WAIT, (uintptr_t)ASMReturnAddress(), RT_SRC_POS_ARGS);
#else
    int vrc = RTSemRWRequestRead(m->sem, RT_INDEFINITE_WAIT);
#endif
    AssertRC(vrc);
}

/*virtual*/ void RWLockHandle::unlockRead()
{
    int vrc = RTSemRWReleaseRead(m->sem);
    AssertRC(vrc);
}

/*virtual*/ uint32_t RWLockHandle::writeLockLevel() const
{
    /* Note! This does not include read recursions done by the writer! */
    return RTSemRWGetWriteRecursion(m->sem);
}

#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
/*virtual*/ const char* RWLockHandle::describe() const
{
    return m->strDescription.c_str();
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
// WriteLockHandle
//
////////////////////////////////////////////////////////////////////////////////

struct WriteLockHandle::Data
{
    Data()
    { }

    mutable RTCRITSECT          sem;
    VBoxLockingClass   lockClass;

#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    com::Utf8Str                strDescription;
#endif
};

WriteLockHandle::WriteLockHandle(VBoxLockingClass lockClass)
{
    m = new Data;

    m->lockClass = lockClass;

#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    m->strDescription = com::Utf8StrFmt("crit %RCv", this);
    int vrc = RTCritSectInitEx(&m->sem, 0/*fFlags*/, g_mapLockValidationClasses[lockClass], RTLOCKVAL_SUB_CLASS_ANY, NULL);
#else
    int vrc = RTCritSectInitEx(&m->sem, 0/*fFlags*/, NIL_RTLOCKVALCLASS, RTLOCKVAL_SUB_CLASS_ANY, NULL);
#endif
    AssertRC(vrc);
}

WriteLockHandle::~WriteLockHandle()
{
    RTCritSectDelete(&m->sem);
    delete m;
}

/*virtual*/ bool WriteLockHandle::isWriteLockOnCurrentThread() const
{
    return RTCritSectIsOwner(&m->sem);
}

/*virtual*/ void WriteLockHandle::lockWrite(LOCKVAL_SRC_POS_DECL)
{
#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    RTCritSectEnterDebug(&m->sem, (uintptr_t)ASMReturnAddress(), RT_SRC_POS_ARGS);
#else
    RTCritSectEnter(&m->sem);
#endif
}

/*virtual*/ void WriteLockHandle::unlockWrite()
{
    RTCritSectLeave(&m->sem);
}

/*virtual*/ void WriteLockHandle::lockRead(LOCKVAL_SRC_POS_DECL)
{
    lockWrite(LOCKVAL_SRC_POS_ARGS);
}

/*virtual*/ void WriteLockHandle::unlockRead()
{
    unlockWrite();
}

/*virtual*/ uint32_t WriteLockHandle::writeLockLevel() const
{
    return RTCritSectGetRecursion(&m->sem);
}

#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
/*virtual*/ const char* WriteLockHandle::describe() const
{
    return m->strDescription.c_str();
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
// AutoLockBase
//
////////////////////////////////////////////////////////////////////////////////

typedef std::vector<LockHandle*> HandlesVector;
typedef std::vector<uint32_t> CountsVector;

struct AutoLockBase::Data
{
    Data(size_t cHandles
#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
         , const char *pcszFile_,
         unsigned uLine_,
         const char *pcszFunction_
#endif
        )
        : fIsLocked(false),
          aHandles(cHandles),       // size of array
          acUnlockedInLeave(cHandles)
#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
          , pcszFile(pcszFile_),
          uLine(uLine_),
          pcszFunction(pcszFunction_)
#endif
    {
        for (uint32_t i = 0; i < cHandles; ++i)
        {
            acUnlockedInLeave[i] = 0;
            aHandles[i] = NULL;
        }
    }

    bool            fIsLocked;          // if true, then all items in aHandles are locked by this AutoLock and
                                        // need to be unlocked in the destructor
    HandlesVector   aHandles;           // array (vector) of LockHandle instances; in the case of AutoWriteLock
                                        // and AutoReadLock, there will only be one item on the list; with the
                                        // AutoMulti* derivatives, there will be multiple
    CountsVector    acUnlockedInLeave;  // for each lock handle, how many times the handle was unlocked in leave(); otherwise 0

#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    // information about where the lock occured (passed down from the AutoLock classes)
    const char      *pcszFile;
    unsigned        uLine;
    const char      *pcszFunction;
#endif
};

AutoLockBase::AutoLockBase(uint32_t cHandles
                           COMMA_LOCKVAL_SRC_POS_DECL)
{
    m = new Data(cHandles
                 COMMA_LOCKVAL_SRC_POS_ARGS);
}

AutoLockBase::AutoLockBase(uint32_t cHandles,
                           LockHandle *pHandle
                           COMMA_LOCKVAL_SRC_POS_DECL)
{
    Assert(cHandles == 1);
    m = new Data(1
                 COMMA_LOCKVAL_SRC_POS_ARGS);
    m->aHandles[0] = pHandle;
}

AutoLockBase::~AutoLockBase()
{
    delete m;
}

/**
 * Requests ownership of all contained lock handles by calling
 * the pure virtual callLockImpl() function on each of them,
 * which must be implemented by the descendant class; in the
 * implementation, AutoWriteLock will request a write lock
 * whereas AutoReadLock will request a read lock.
 *
 * Does *not* modify the lock counts in the member variables.
 */
void AutoLockBase::callLockOnAllHandles()
{
    for (HandlesVector::iterator it = m->aHandles.begin();
         it != m->aHandles.end();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
            // call virtual function implemented in AutoWriteLock or AutoReadLock
            this->callLockImpl(*pHandle);
    }
}

/**
 * Releases ownership of all contained lock handles by calling
 * the pure virtual callUnlockImpl() function on each of them,
 * which must be implemented by the descendant class; in the
 * implementation, AutoWriteLock will release a write lock
 * whereas AutoReadLock will release a read lock.
 *
 * Does *not* modify the lock counts in the member variables.
 */
void AutoLockBase::callUnlockOnAllHandles()
{
    // unlock in reverse order!
    for (HandlesVector::reverse_iterator it = m->aHandles.rbegin();
         it != m->aHandles.rend();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
            // call virtual function implemented in AutoWriteLock or AutoReadLock
            this->callUnlockImpl(*pHandle);
    }
}

/**
 * Destructor implementation that can also be called explicitly, if required.
 * Restores the exact state before the AutoLock was created; that is, unlocks
 * all contained semaphores and might actually lock them again if leave()
 * was called during the AutoLock's lifetime.
 */
void AutoLockBase::cleanup()
{
    bool fAnyUnlockedInLeave = false;

    uint32_t i = 0;
    for (HandlesVector::iterator it = m->aHandles.begin();
         it != m->aHandles.end();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            if (m->acUnlockedInLeave[i])
            {
                // there was a leave() before the destruction: then restore the
                // lock level that might have been set by locks other than our own
                if (m->fIsLocked)
                {
                    --m->acUnlockedInLeave[i];
                    fAnyUnlockedInLeave = true;
                }
                for (; m->acUnlockedInLeave[i]; --m->acUnlockedInLeave[i])
                    callLockImpl(*pHandle);
            }
        }
        ++i;
    }

    if (m->fIsLocked && !fAnyUnlockedInLeave)
        callUnlockOnAllHandles();
}

/**
 * Requests ownership of all contained semaphores. Public method that can
 * only be called once and that also gets called by the AutoLock constructors.
 */
void AutoLockBase::acquire()
{
    AssertMsg(!m->fIsLocked, ("m->fIsLocked is true, attempting to lock twice!"));
    callLockOnAllHandles();
    m->fIsLocked = true;
}

/**
 * Releases ownership of all contained semaphores. Public method.
 */
void AutoLockBase::release()
{
    AssertMsg(m->fIsLocked, ("m->fIsLocked is false, cannot release!"));
    callUnlockOnAllHandles();
    m->fIsLocked = false;
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoReadLock
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Release all read locks acquired by this instance through the #lock()
 * call and destroys the instance.
 *
 * Note that if there there are nested #lock() calls without the
 * corresponding number of #unlock() calls when the destructor is called, it
 * will assert. This is because having an unbalanced number of nested locks
 * is a program logic error which must be fixed.
 */
/*virtual*/ AutoReadLock::~AutoReadLock()
{
    LockHandle *pHandle = m->aHandles[0];

    if (pHandle)
    {
        if (m->fIsLocked)
            callUnlockImpl(*pHandle);
    }
}

/**
 * Implementation of the pure virtual declared in AutoLockBase.
 * This gets called by AutoLockBase.acquire() to actually request
 * the semaphore; in the AutoReadLock implementation, we request
 * the semaphore in read mode.
 */
/*virtual*/ void AutoReadLock::callLockImpl(LockHandle &l)
{
#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    l.lockRead(m->pcszFile, m->uLine, m->pcszFunction);
#else
    l.lockRead();
#endif
}

/**
 * Implementation of the pure virtual declared in AutoLockBase.
 * This gets called by AutoLockBase.release() to actually release
 * the semaphore; in the AutoReadLock implementation, we release
 * the semaphore in read mode.
 */
/*virtual*/ void AutoReadLock::callUnlockImpl(LockHandle &l)
{
    l.unlockRead();
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoWriteLockBase
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Implementation of the pure virtual declared in AutoLockBase.
 * This gets called by AutoLockBase.acquire() to actually request
 * the semaphore; in the AutoWriteLock implementation, we request
 * the semaphore in write mode.
 */
/*virtual*/ void AutoWriteLockBase::callLockImpl(LockHandle &l)
{
#ifdef VBOX_WITH_MAIN_LOCK_VALIDATION
    l.lockWrite(m->pcszFile, m->uLine, m->pcszFunction);
#else
    l.lockWrite();
#endif
}

/**
 * Implementation of the pure virtual declared in AutoLockBase.
 * This gets called by AutoLockBase.release() to actually release
 * the semaphore; in the AutoWriteLock implementation, we release
 * the semaphore in write mode.
 */
/*virtual*/ void AutoWriteLockBase::callUnlockImpl(LockHandle &l)
{
    l.unlockWrite();
}

/**
 * Causes the current thread to completely release the write lock to make
 * the managed semaphore immediately available for locking by other threads.
 *
 * This implies that all nested write locks on the semaphore will be
 * released, even those that were acquired through the calls to #lock()
 * methods of all other AutoWriteLock/AutoReadLock instances managing the
 * <b>same</b> read/write semaphore.
 *
 * After calling this method, the only method you are allowed to call is
 * #enter(). It will acquire the write lock again and restore the same
 * level of nesting as it had before calling #leave().
 *
 * If this instance is destroyed without calling #enter(), the destructor
 * will try to restore the write lock level that existed when #leave() was
 * called minus the number of nested #lock() calls made on this instance
 * itself. This is done to preserve lock levels of other
 * AutoWriteLock/AutoReadLock instances managing the same semaphore (if
 * any). Tiis also means that the destructor may indefinitely block if a
 * write or a read lock is owned by some other thread by that time.
 */
void AutoWriteLockBase::leave()
{
    AssertMsg(m->fIsLocked, ("m->fIsLocked is false, cannot leave()!"));

    // unlock in reverse order!
    uint32_t i = m->aHandles.size();
    for (HandlesVector::reverse_iterator it = m->aHandles.rbegin();
         it != m->aHandles.rend();
         ++it)
    {
        --i;            // array index is zero based, decrement with every loop since we iterate backwards
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            AssertMsg(m->acUnlockedInLeave[i] == 0, ("m->cUnlockedInLeave[%d] is %d, must be 0! Called leave() twice?", i, m->acUnlockedInLeave[i]));
            m->acUnlockedInLeave[i] = pHandle->writeLockLevel();
            AssertMsg(m->acUnlockedInLeave[i] >= 1, ("m->cUnlockedInLeave[%d] is %d, must be >=1!", i, m->acUnlockedInLeave[i]));

            for (uint32_t left = m->acUnlockedInLeave[i];
                 left;
                 --left)
                callUnlockImpl(*pHandle);
        }
    }
}

/**
 * Causes the current thread to restore the write lock level after the
 * #leave() call. This call will indefinitely block if another thread has
 * successfully acquired a write or a read lock on the same semaphore in
 * between.
 */
void AutoWriteLockBase::enter()
{
    AssertMsg(m->fIsLocked, ("m->fIsLocked is false, cannot enter()!"));

    uint32_t i = 0;
    for (HandlesVector::iterator it = m->aHandles.begin();
         it != m->aHandles.end();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            AssertMsg(m->acUnlockedInLeave[i] != 0, ("m->cUnlockedInLeave[%d] is 0! enter() without leave()?", i));

            for (; m->acUnlockedInLeave[i]; --m->acUnlockedInLeave[i])
                callLockImpl(*pHandle);
        }
        ++i;
    }
}

/**
 * Same as #leave() but checks if the current thread actally owns the lock
 * and only proceeds in this case. As a result, as opposed to #leave(),
 * doesn't assert when called with no lock being held.
 */
void AutoWriteLockBase::maybeLeave()
{
    // unlock in reverse order!
    uint32_t i = m->aHandles.size();
    for (HandlesVector::reverse_iterator it = m->aHandles.rbegin();
         it != m->aHandles.rend();
         ++it)
    {
        --i;            // array index is zero based, decrement with every loop since we iterate backwards
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            if (pHandle->isWriteLockOnCurrentThread())
            {
                m->acUnlockedInLeave[i] = pHandle->writeLockLevel();
                AssertMsg(m->acUnlockedInLeave[i] >= 1, ("m->cUnlockedInLeave[%d] is %d, must be >=1!", i, m->acUnlockedInLeave[i]));

                for (uint32_t left = m->acUnlockedInLeave[i];
                     left;
                     --left)
                    callUnlockImpl(*pHandle);
            }
        }
        ++i;
    }
}

/**
 * Same as #enter() but checks if the current thread actally owns the lock
 * and only proceeds if not. As a result, as opposed to #enter(), doesn't
 * assert when called with the lock already being held.
 */
void AutoWriteLockBase::maybeEnter()
{
    uint32_t i = 0;
    for (HandlesVector::iterator it = m->aHandles.begin();
         it != m->aHandles.end();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            if (!pHandle->isWriteLockOnCurrentThread())
            {
                for (; m->acUnlockedInLeave[i]; --m->acUnlockedInLeave[i])
                    callLockImpl(*pHandle);
            }
        }
        ++i;
    }
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoWriteLock
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Attaches another handle to this auto lock instance.
 *
 * The previous object's lock is completely released before the new one is
 * acquired. The lock level of the new handle will be the same. This
 * also means that if the lock was not acquired at all before #attach(), it
 * will not be acquired on the new handle too.
 *
 * @param aHandle   New handle to attach.
 */
void AutoWriteLock::attach(LockHandle *aHandle)
{
    LockHandle *pHandle = m->aHandles[0];

    /* detect simple self-reattachment */
    if (pHandle != aHandle)
    {
        bool fWasLocked = m->fIsLocked;

        cleanup();

        m->aHandles[0] = aHandle;
        m->fIsLocked = fWasLocked;

        if (aHandle)
            if (fWasLocked)
                callLockImpl(*aHandle);
    }
}

/**
 * Returns @c true if the current thread holds a write lock on the managed
 * read/write semaphore. Returns @c false if the managed semaphore is @c
 * NULL.
 *
 * @note Intended for debugging only.
 */
bool AutoWriteLock::isWriteLockOnCurrentThread() const
{
    return m->aHandles[0] ? m->aHandles[0]->isWriteLockOnCurrentThread() : false;
}

 /**
 * Returns the current write lock level of the managed smaphore. The lock
 * level determines the number of nested #lock() calls on the given
 * semaphore handle. Returns @c 0 if the managed semaphore is @c
 * NULL.
 *
 * Note that this call is valid only when the current thread owns a write
 * lock on the given semaphore handle and will assert otherwise.
 *
 * @note Intended for debugging only.
 */
uint32_t AutoWriteLock::writeLockLevel() const
{
    return m->aHandles[0] ? m->aHandles[0]->writeLockLevel() : 0;
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoMultiWriteLock*
//
////////////////////////////////////////////////////////////////////////////////

AutoMultiWriteLock2::AutoMultiWriteLock2(Lockable *pl1,
                                         Lockable *pl2
                                         COMMA_LOCKVAL_SRC_POS_DECL)
    : AutoWriteLockBase(2
                        COMMA_LOCKVAL_SRC_POS_ARGS)
{
    if (pl1)
        m->aHandles[0] = pl1->lockHandle();
    if (pl2)
        m->aHandles[1] = pl2->lockHandle();
    acquire();
}

AutoMultiWriteLock2::AutoMultiWriteLock2(LockHandle *pl1,
                                         LockHandle *pl2
                                         COMMA_LOCKVAL_SRC_POS_DECL)
    : AutoWriteLockBase(2
                        COMMA_LOCKVAL_SRC_POS_ARGS)
{
    m->aHandles[0] = pl1;
    m->aHandles[1] = pl2;
    acquire();
}

AutoMultiWriteLock3::AutoMultiWriteLock3(Lockable *pl1,
                                         Lockable *pl2,
                                         Lockable *pl3
                                         COMMA_LOCKVAL_SRC_POS_DECL)
    : AutoWriteLockBase(3
                        COMMA_LOCKVAL_SRC_POS_ARGS)
{
    if (pl1)
        m->aHandles[0] = pl1->lockHandle();
    if (pl2)
        m->aHandles[1] = pl2->lockHandle();
    if (pl3)
        m->aHandles[2] = pl3->lockHandle();
    acquire();
}

AutoMultiWriteLock3::AutoMultiWriteLock3(LockHandle *pl1,
                                         LockHandle *pl2,
                                         LockHandle *pl3
                                         COMMA_LOCKVAL_SRC_POS_DECL)
    : AutoWriteLockBase(3
                        COMMA_LOCKVAL_SRC_POS_ARGS)
{
    m->aHandles[0] = pl1;
    m->aHandles[1] = pl2;
    m->aHandles[2] = pl3;
    acquire();
}

} /* namespace util */
/* vi: set tabstop=4 shiftwidth=4 expandtab: */
