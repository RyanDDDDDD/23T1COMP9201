#include "opt-synchprobs.h"
#include "kitchen.h"
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <test.h>
#include <thread.h>
#include <synch.h>




/*
 * ********************************************************************
 * INSERT ANY GLOBAL VARIABLES YOU REQUIRE HERE
 * ********************************************************************
 */

int pot_size;
struct lock *mutex;

struct cv *customer;
struct cv *cook;

/*
 * initialise_kitchen: 
 *
 * This function is called during the initialisation phase of the
 * kitchen, i.e.before any threads are created.
 *
 * Initialise any global variables or create any synchronisation
 * primitives here.
 * 
 * The function returns 0 on success or non-zero on failure.
 */

int initialise_kitchen()
{       
        pot_size = 0;
        mutex = lock_create("mutex");
        if (mutex == NULL){
                return ENOMEM;
                // allocation failure
        }

        customer = cv_create("customer");
        if (customer == NULL){
                return ENOMEM;
                // allocation failure
        }

        cook = cv_create("cook");
        if (cook == NULL){
                return ENOMEM;
                // allocation failure
        }

        return 0;
}

/*
 * cleanup_kitchen:
 *
 * This function is called after the dining threads and cook thread
 * have exited the system. You should deallocated any memory allocated
 * by the initialisation phase (e.g. destroy any synchronisation
 * primitives).
 */

void cleanup_kitchen()
{
        cv_destroy(cook);
        cv_destroy(customer);
        lock_destroy(mutex);
}


/*
 * do_cooking:
 *
 * This function is called repeatedly by the cook thread to provide
 * enough soup to dining customers. It creates soup by calling
 * cook_soup_in_pot().
 *
 * It should wait until the pot is empty before calling
 * cook_soup_in_pot().
 *
 * It should wake any dining threads waiting for more soup.
 */

void do_cooking()
{
        lock_acquire(mutex);

        while(pot_size > 0){  
                cv_wait(cook,mutex);
        }

        cook_soup_in_pot();
        pot_size = POTSIZE_IN_SERVES;

        cv_broadcast(customer,mutex);

        lock_release(mutex);
}

/*
 * fill_bowl:
 *
 * This function is called repeatedly by dining threads to obtain soup
 * to satify their hunger. Dining threads fill their bowl by calling
 * get_serving_from_pot().
 *
 * It should wait until there is soup in the pot before calling
 * get_serving_from_pot().
 *
 * get_serving_from_pot() should be called mutually exclusively as
 * only one thread can fill their bowl at a time.
 *
 * fill_bowl should wake the cooking thread if there is no soup left
 * in the pot.
 */

void fill_bowl()
{
        lock_acquire(mutex);
       
        while(pot_size == 0){
                cv_wait(customer,mutex);
        }

        get_serving_from_pot();
        pot_size = pot_size - 1;

        cv_signal(cook,mutex);
        
        lock_release(mutex);
}
