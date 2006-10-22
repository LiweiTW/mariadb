/* Copyright (C) 2006 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <tap.h>

#include <my_global.h>
#include <my_sys.h>
#include <my_atomic.h>
#include <lf.h>

volatile uint32 a32,b32,c32, N;
my_atomic_rwlock_t rwl;
LF_ALLOCATOR lf_allocator;
LF_HASH lf_hash;

/* add and sub a random number in a loop. Must get 0 at the end */
pthread_handler_t test_atomic_add_handler(void *arg)
{
  int    m= (*(int *)arg)/2;
  int32 x;
  for (x= ((int)(intptr)(&m)); m ; m--)
  {
    x= (x*m+0x87654321) & INT_MAX32;
    my_atomic_rwlock_wrlock(&rwl);
    my_atomic_add32(&a32, x);
    my_atomic_rwlock_wrunlock(&rwl);

    my_atomic_rwlock_wrlock(&rwl);
    my_atomic_add32(&a32, -x);
    my_atomic_rwlock_wrunlock(&rwl);
  }
  return 0;
}

/*
  1. generate thread number 0..N-1 from b32
  2. add it to a32
  3. swap thread numbers in c32
  4. (optionally) one more swap to avoid 0 as a result
  5. subtract result from a32
  must get 0 in a32 at the end
*/
pthread_handler_t test_atomic_fas_handler(void *arg)
{
  int    m= *(int *)arg;
  uint32  x= my_atomic_add32(&b32, 1);

  my_atomic_add32(&a32, x);

  for (; m ; m--)
  {
    my_atomic_rwlock_wrlock(&rwl);
    x= my_atomic_fas32(&c32, x);
    my_atomic_rwlock_wrunlock(&rwl);
  }

  if (!x)
  {
    my_atomic_rwlock_wrlock(&rwl);
    x= my_atomic_fas32(&c32, x);
    my_atomic_rwlock_wrunlock(&rwl);
  }

  my_atomic_rwlock_wrlock(&rwl);
  my_atomic_add32(&a32, -x);
  my_atomic_rwlock_wrunlock(&rwl);

  return 0;
}

/*
  same as test_atomic_add_handler, but my_atomic_add32 is emulated with
  my_atomic_cas32 - notice that the slowdown is proportional to the
  number of CPUs
*/
pthread_handler_t test_atomic_cas_handler(void *arg)
{
  int    m= (*(int *)arg)/2, ok= 0;
  int32 x, y;
  for (x= ((int)(intptr)(&m)); m ; m--)
  {
    my_atomic_rwlock_wrlock(&rwl);
    y= my_atomic_load32(&a32);
    my_atomic_rwlock_wrunlock(&rwl);
    x= (x*m+0x87654321) & INT_MAX32;
    do {
      my_atomic_rwlock_wrlock(&rwl);
      ok= my_atomic_cas32(&a32, &y, y+x);
      my_atomic_rwlock_wrunlock(&rwl);
    } while (!ok) ;
    do {
      my_atomic_rwlock_wrlock(&rwl);
      ok= my_atomic_cas32(&a32, &y, y-x);
      my_atomic_rwlock_wrunlock(&rwl);
    } while (!ok) ;
  }
  return 0;
}

/*
  pin allocator - alloc and release an element in a loop
*/
pthread_handler_t test_lf_pinbox(void *arg)
{
  int    m= *(int *)arg;
  int32 x= 0;
  LF_PINS *pins;

  pins= lf_pinbox_get_pins(&lf_allocator.pinbox);

  for (x= ((int)(intptr)(&m)); m ; m--)
  {
    lf_pinbox_put_pins(pins);
    pins= lf_pinbox_get_pins(&lf_allocator.pinbox);
  }
  lf_pinbox_put_pins(pins);
  return 0;
}

typedef union {
  int32 data;
  void *not_used;
} TLA;

pthread_handler_t test_lf_alloc(void *arg)
{
  int    m= (*(int *)arg)/2;
  int32 x,y= 0;
  LF_PINS *pins;

  pins= lf_alloc_get_pins(&lf_allocator);

  for (x= ((int)(intptr)(&m)); m ; m--)
  {
    TLA *node1, *node2;
    x= (x*m+0x87654321) & INT_MAX32;
    node1= (TLA *)lf_alloc_new(pins);
    node1->data= x;
    y+= node1->data;
    node1->data= 0;
    node2= (TLA *)lf_alloc_new(pins);
    node2->data= x;
    y-= node2->data;
    node2->data= 0;
    lf_alloc_free(pins, node1);
    lf_alloc_free(pins, node2);
  }
  lf_alloc_put_pins(pins);
  my_atomic_rwlock_wrlock(&rwl);
  my_atomic_add32(&a32, y);

  if (my_atomic_add32(&N, -1) == 1)
  {
    diag("%d mallocs, %d pins in stack",
         lf_allocator.mallocs, lf_allocator.pinbox.pins_in_stack);
#ifdef MY_LF_EXTRA_DEBUG
    a32|= lf_allocator.mallocs - lf_alloc_in_pool(&lf_allocator);
#endif
  }
  my_atomic_rwlock_wrunlock(&rwl);
  return 0;
}

#define N_TLH 1000
pthread_handler_t test_lf_hash(void *arg)
{
  int    m= (*(int *)arg)/(2*N_TLH);
  int32 x,y,z,sum= 0, ins= 0;
  LF_PINS *pins;

  pins= lf_hash_get_pins(&lf_hash);

  for (x= ((int)(intptr)(&m)); m ; m--)
  {
    int i;
    y= x;
    for (i= 0; i < N_TLH; i++)
    {
      x= (x*(m+i)+0x87654321) & INT_MAX32;
      z= (x<0) ? -x : x;
      if (lf_hash_insert(&lf_hash, pins, &z))
      {
        sum+= z;
        ins++;
      }
    }
    for (i= 0; i < N_TLH; i++)
    {
      y= (y*(m+i)+0x87654321) & INT_MAX32;
      z= (y<0) ? -y : y;
      if (lf_hash_delete(&lf_hash, pins, (uchar *)&z, sizeof(z)))
        sum-= z;
    }
  }
  lf_hash_put_pins(pins);
  my_atomic_rwlock_wrlock(&rwl);
  my_atomic_add32(&a32, sum);
  my_atomic_add32(&b32, ins);
  my_atomic_add32(&a32, y);

  if (my_atomic_add32(&N, -1) == 1)
  {
    diag("%d mallocs, %d pins in stack, %d hash size, %d inserts",
         lf_hash.alloc.mallocs, lf_hash.alloc.pinbox.pins_in_stack,
         lf_hash.size, b32);
    a32|= lf_hash.count;
  }
  my_atomic_rwlock_wrunlock(&rwl);
  return 0;
}

void test_atomic(const char *test, pthread_handler handler, int n, int m)
{
  pthread_t *threads;
  ulonglong now= my_getsystime();
  int i;

  a32= 0;
  b32= 0;
  c32= 0;

  threads= (pthread_t *)my_malloc(sizeof(void *)*n, MYF(0));
  if (!threads)
  {
    diag("Out of memory");
    abort();
  }

  diag("Testing %s with %d threads, %d iterations... ", test, n, m);
  N= n;
  for (i= 0 ; i < n ; i++)
  {
    if (pthread_create(threads+i, 0, handler, &m) != 0)
    {
      diag("Could not create thread");
      abort();
    }
  }
  for (i= 0 ; i < n ; i++)
    pthread_join(threads[i], 0);
  now= my_getsystime()-now;
err:
  ok(a32 == 0, "tested %s in %g secs (%d)", test, ((double)now)/1e7, a32);
  my_free((void *)threads, MYF(0));
}

int main()
{
  int err;

  my_init();

  diag("N CPUs: %d, atomic ops: %s", my_getncpus(), MY_ATOMIC_MODE);
  err= my_atomic_initialize();

  plan(7);
  ok(err == 0, "my_atomic_initialize() returned %d", err);

  my_atomic_rwlock_init(&rwl);
  lf_alloc_init(&lf_allocator, sizeof(TLA), offsetof(TLA, not_used));
  lf_hash_init(&lf_hash, sizeof(int), LF_HASH_UNIQUE, 0, sizeof(int), 0,
               &my_charset_bin);

#ifdef MY_ATOMIC_MODE_RWLOCKS
#define CYCLES 10000
#else
#define CYCLES 1000000
#endif
#define THREADS 100

  test_atomic("my_atomic_add32",  test_atomic_add_handler,    THREADS,CYCLES);
  test_atomic("my_atomic_fas32",  test_atomic_fas_handler,    THREADS,CYCLES);
  test_atomic("my_atomic_cas32",  test_atomic_cas_handler,    THREADS,CYCLES);
  test_atomic("lf_pinbox",        test_lf_pinbox,             THREADS,CYCLES);
  test_atomic("lf_alloc",         test_lf_alloc,              THREADS,CYCLES);
  test_atomic("lf_hash",          test_lf_hash,               THREADS,CYCLES);

  lf_hash_destroy(&lf_hash);
  lf_alloc_destroy(&lf_allocator);

  my_atomic_rwlock_destroy(&rwl);
  my_end(0);
  return exit_status();
}

