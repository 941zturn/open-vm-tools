/*********************************************************
 * Copyright (C) 2010 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

#include "vmware.h"
#include "str.h"
#include "util.h"
#include "userlock.h"
#include "ulInt.h"

#define MXUSER_BARRIER_SIGNATURE 0x52524142 // 'BARR' in memory

struct BarrierContext
{
   uint32           count;    // Number of threads currently in this context
   MXUserCondVar   *condVar;  // Threads within this context are parked here
};

typedef struct BarrierContext BarrierContext;

struct MXUserBarrier
{
   MXUserHeader     header;        // Barrier's ID information
   MXUserExclLock  *lock;          // Barrier's (internal) lock
   Bool             emptying;      // Barrier is emptying
   uint32           configCount;   // Hold until this many threads arrive.
   uint32           curContext;    // Normal arrivals go to this context
   BarrierContext   contexts[2];   // The normal and abnormal contexts
};


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserDumpBarrier --
 *
 *      Dump a barrier.
 *
 * Results:
 *      A dump.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
MXUserDumpBarrier(MXUserHeader *header)  // IN:
{
   MXUserBarrier *barrier = (MXUserBarrier *) header;

   Warning("%s: Barrier @ 0x%p\n", __FUNCTION__, barrier);

   Warning("\tsignature 0x%X\n", barrier->header.signature);
   Warning("\tname %s\n", barrier->header.name);
   Warning("\trank 0x%X\n", barrier->header.rank);

   Warning("\tlock %p\n", barrier->lock);
   Warning("\tconfigured count %u\n", barrier->configCount);
   Warning("\tcurrent context %u\n", barrier->curContext);

   Warning("\tcontext[0] count %u\n", barrier->contexts[0].count);
   Warning("\tcontext[0] condVar 0x%p\n", &barrier->contexts[0].condVar);

   Warning("\tcontext[1] count %u\n", barrier->contexts[1].count);
   Warning("\tcontext[1] condVar 0x%p\n", &barrier->contexts[1].condVar);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_CreateBarrier --
 *
 *      Create a computational barrier.
 *
 *      The barriers are self regenerating - they do not need to be
 *      initialized or reset after creation.
 *
 * Results:
 *      A pointer to a barrier.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

MXUserBarrier *
MXUser_CreateBarrier(const char *userName,  // IN:
                     MX_Rank rank,          // IN:
                     uint32 count)          // IN:
{
   char *properName;
   MXUserBarrier *barrier;

   ASSERT(count);

   barrier = Util_SafeCalloc(1, sizeof(*barrier));

   if (userName == NULL) {
      properName = Str_SafeAsprintf(NULL, "Barrier-%p", GetReturnAddress());
   } else {
      properName = Util_SafeStrdup(userName);
   }

   barrier->lock = MXUser_CreateExclLock(properName, rank);

   if (barrier->lock == NULL) {
      free(properName);
      free(barrier);

      return NULL;
   }


   barrier->contexts[0].condVar = MXUser_CreateCondVarExclLock(barrier->lock);
   barrier->contexts[1].condVar = MXUser_CreateCondVarExclLock(barrier->lock);

   if ((barrier->contexts[0].condVar == NULL) ||
       (barrier->contexts[1].condVar == NULL)) {
      MXUser_DestroyCondVar(barrier->contexts[0].condVar);
      MXUser_DestroyCondVar(barrier->contexts[1].condVar);
      MXUser_DestroyExclLock(barrier->lock);

      free(properName);
      free(barrier);

      return NULL;
   }

   barrier->configCount = count;
   barrier->emptying = FALSE;
   barrier->curContext = 0;

   barrier->header.name = properName;
   barrier->header.signature = MXUSER_BARRIER_SIGNATURE;
   barrier->header.rank = rank;
   barrier->header.dumpFunc = MXUserDumpBarrier;

#if defined(MXUSER_STATS)
   barrier->header.statsFunc = NULL;
   barrier->header.identifier = MXUserAllocID();
#endif

   return barrier;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_DestroyBarrier --
 *
 *      Destroy a barrier.
 *
 * Results:
 *      The barrier is destroyed. Don't try to use the pointer again.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_DestroyBarrier(MXUserBarrier *barrier)  // IN:
{
   if (LIKELY(barrier != NULL)) {
      ASSERT(barrier->header.signature == MXUSER_BARRIER_SIGNATURE);

      if ((barrier->contexts[0].count != 0) ||
          (barrier->contexts[1].count != 0)) {
         MXUserDumpAndPanic(&barrier->header,
                            "%s: Attempted destroy on barrier while in use\n",
                            __FUNCTION__);
      }

      MXUser_DestroyCondVar(barrier->contexts[0].condVar);
      MXUser_DestroyCondVar(barrier->contexts[1].condVar);
      MXUser_DestroyExclLock(barrier->lock);

      barrier->header.signature = 0;  // just in case...
      free((void *) barrier->header.name);  // avoid const warnings
      barrier->header.name = NULL;
      free(barrier);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_EnterBarrier --
 *
 *      Enter a barrier
 *
 *      All threads entering the barrier will be suspended until the number
 *      threads that have entered reaches the configured number upon which
 *      time all of the threads will return from this routine.
 *
 *      "Nobody comes out until everone goes in."
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The caller may sleep.
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_EnterBarrier(MXUserBarrier *barrier)  // IN/OUT:
{
   BarrierContext *ptr;

   ASSERT(barrier && (barrier->header.signature == MXUSER_BARRIER_SIGNATURE));

   MXUser_AcquireExclLock(barrier->lock);

   if (barrier->emptying) {
      uint32 other = (barrier->curContext + 1) & 0x1;

      /*
       * An abnormal entry. A thread has entered while the barrier is emptying.
       * Park the thread on a condVar - not the one currently involved with
       * the emptying - and account for the thread.
       *
       * The last thread out of the barrier will switch the barrier to using
       * the alternative condVar and things will progress properly on their
       * own.
       */

      ptr = &barrier->contexts[other];

      ptr->count++;

      MXUser_WaitCondVarExclLock(barrier->lock, ptr->condVar);
   } else {
      /*
       * A normal entry. All threads but the last are parked on a condVar;
       * the last thread in does a broadcast to kick the threads out of the
       * condVar.
       *
       * The last thread out of the barrier cleans up the barrier and resets
       * it for the next time.
       */

      ptr = &barrier->contexts[barrier->curContext];

      ptr->count++;

      barrier->emptying = (ptr->count == barrier->configCount);

      if (barrier->emptying) {
         /* The last thread has entered; release the other threads */
         MXUser_BroadcastCondVar(ptr->condVar);
      } else {
         /* Not the last thread in... sleep until the last thread appears */
         MXUser_WaitCondVarExclLock(barrier->lock, ptr->condVar);
      }
   }

   ptr->count--;

   if (ptr->count == 0) {
      barrier->emptying = FALSE;
      barrier->curContext = (barrier->curContext + 1) & 0x1;
   }

   MXUser_ReleaseExclLock(barrier->lock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_CreateSingletonBarrier --
 *
 *      Ensures that the specified backing object (Atomic_Ptr) contains a
 *      barrier. This is useful for modules that need to protect something
 *      with a barrier but don't have an existing Init() entry point where a
 *      barrier can be created.
 *
 * Results:
 *      A pointer to the requested barrier.
 *
 * Side effects:
 *      Generally the barrier's resources are intentionally leaked (by design).
 *
 *-----------------------------------------------------------------------------
 */

MXUserBarrier *
MXUser_CreateSingletonBarrier(Atomic_Ptr *barrierStorage,  // IN/OUT:
                              const char *name,            // IN:
                              MX_Rank rank,                // IN:
                              uint32 count)                // IN:
{
   MXUserBarrier *barrier;

   ASSERT(barrierStorage);

   barrier = (MXUserBarrier *) Atomic_ReadPtr(barrierStorage);

   if (UNLIKELY(barrier == NULL)) {
      MXUserBarrier *newBarrier = MXUser_CreateBarrier(name, rank, count);

      barrier = (MXUserBarrier *) Atomic_ReadIfEqualWritePtr(barrierStorage,
                                                             NULL,
                                                           (void *) newBarrier);

      if (barrier) {
         MXUser_DestroyBarrier(newBarrier);
      } else {
         barrier = (MXUserBarrier *) Atomic_ReadPtr(barrierStorage);
      }
   }

   return barrier;
}
