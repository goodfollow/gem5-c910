#ifndef __TYPES_H
#define __TYPES_H

#ifndef COMPLEX_FLOAT
  #define COMPLEX_FLOAT double _Complex
#endif

#ifndef REAL_FLOAT
  #define REAL_FLOAT double
#endif

#ifndef MAX_UNSIGNED
  #define MAX_UNSIGNED unsigned long long
#endif

/* Use literal imaginary unit since local complex.h shadows system one */
#ifndef IMAGINARY
  #define IMAGINARY 1.0iF
#endif

#endif
