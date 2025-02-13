! Copyright 2020-2021 Free Software Foundation, Inc.
!
! This program is free software; you can redistribute it and/or modify
! it under the terms of the GNU General Public License as published by
! the Free Software Foundation; either version 3 of the License, or
! (at your option) any later version.
!
! This program is distributed in the hope that it will be useful,
! but WITHOUT ANY WARRANTY; without even the implied warranty of
! MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
! GNU General Public License for more details.
!
! You should have received a copy of the GNU General Public License
! along with this program.  If not, see <http://www.gnu.org/licenses/> .

program multi_kernel_unnamed
  integer, parameter :: length = 3
  integer, dimension(0:length) :: in_arr
  integer :: i, item

  do i = 0, length
    in_arr(i) = i
  end do

  do i = 0, length
    !$omp target teams num_teams(1) thread_limit(1) map(from: in_elem) &
    !$omp  private(item)
      item = in_arr(i) ! kernel-line
    !$omp end target teams
  end do

  !$omp single
    i = 0 ! line-after-kernel
  !$omp end single
end program multi_kernel_unnamed
