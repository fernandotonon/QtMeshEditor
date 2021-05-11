/**
    @file Euler.h
    @brief %Euler class for %Ogre
    @details License: Do whatever you want with it.
    @version 2.3
    @author Kojack
    @author Transporter
    @author Klaim

Extracted From: http://www.ogre3d.org/tikiwiki/tiki-ind ... e=Cookbook
*/
#ifndef HGUARD_OGRE_MATHS_EULER_H
#define HGUARD_OGRE_MATHS_EULER_H

#include <OgreMath.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <OgreMatrix3.h>

namespace Ogre {

    /**
    @class Euler
    @brief Class for %Euler rotations

    <table><tr><td>Yaw is a rotation around the Y axis.</td><td>Pitch is a rotation around the X axis.</td><td>Roll is a rotation around the Z axis.</td></tr>
    <tr><td><img src="http://www.ogre3d.org/tikiwiki/tiki-download_file.php?fileId=2112" /></td><td><img src="http://www.ogre3d.org/tikiwiki/tiki-download_file.php?fileId=2113" /></td><td><img src="http://www.ogre3d.org/tikiwiki/tiki-download_file.php?fileId=2114" /></td></tr></table>
    */
    class Euler
    {
    public:
       /// Default constructor.
       Euler()
          : mYaw(Radian(0.0f)), mPitch(Radian(0.0f)), mRoll(Radian(0.0f)), mChanged(true)
       {
       }

       /**
       @brief Constructor which takes yaw, pitch and roll values.
       @param y Starting value for yaw
       @param p Starting value for pitch
       @param r Starting value for roll
       */
       Euler(const Radian &y, const Radian &p = Radian(0.0f), const Radian &r = Radian(0.0f))
          : mYaw(y), mPitch(p), mRoll(r), mChanged(true)
       {
       }

        /**
       @brief Constructor which takes yaw, pitch and roll values as reals (radians).
       @param y Starting value for yaw [radian]
       @param p Starting value for pitch [radian]
       @param r Starting value for roll [radian]
       */
       Euler(Real y, Real p = 0.0f, Real r = 0.0f)
          : mYaw(Radian(y)), mPitch(Radian(p)), mRoll(Radian(r)), mChanged(true)
       {
       }

       /**
       @brief Default constructor with presets.
       @param quaternion Calculate starting values from this quaternion
       */
       explicit Euler(const Quaternion &quaternion)
       {
          fromQuaternion(quaternion);
       }

       explicit Euler(const Matrix3 &matrix)
       {
          fromMatrix3(matrix);
       }

       /// Get the Yaw angle.
       inline Radian yaw() const { return mYaw; }

       /// Get the Pitch angle.
       inline Radian pitch() const { return mPitch; }

       /// Get the Roll angle.
       inline Radian roll() const { return mRoll; }

       /**
       @brief Set the yaw.
       @param y New value for yaw
       */
       inline Euler &setYaw(Radian y)
       {
          mYaw = y;
          mChanged = true;
          return *this;
       }

       /**
       @brief Set the pitch.
       @param p New value for pitch
       */
       inline Euler &setPitch(Radian p)
       {
          mPitch = p;
          mChanged = true;
          return *this;
       }

       /**
       @brief Set the roll.
       @param r New value for roll
       */
       inline Euler &setRoll(Radian r)
       {
          mRoll = r;
          mChanged = true;
          return *this;
       }

       /**
       @brief Set all rotations at once.
       @param y New value for yaw
       @param p New value for pitch
       @param r New value for roll
       */
       inline Euler &orientation(const Radian &y, const Radian &p, const Radian &r)
       {
          mYaw = y;
          mPitch = p;
          mRoll = r;
          mChanged = true;
          return *this;
       }

       /**
       @brief Apply a relative yaw.
       @param y Angle to add on current yaw
       */
       inline Euler &yaw(const Radian &y)
       {
          mYaw += y;
          mChanged = true;
          return *this;
       }

       /**
       @brief Apply a relative pitch.
       @param p Angle to add on current pitch
       */
       inline Euler &pitch(const Radian &p)
       {
          mPitch += p;
          mChanged = true;
          return *this;
       }

       /**
       @brief Apply a relative roll.
       @param r Angle to add on current roll
       */
       inline Euler &roll(const Radian &r)
       {
          mRoll += r;
          mChanged = true;
          return *this;
       }

       /**
       @brief Apply all relative rotations at once.
       @param y Angle to add on current yaw
       @param p Angle to add on current pitch
       @param r Angle to add on current roll
       */
       inline Euler &rotate(const Radian &y, const Radian &p, const Radian &r)
       {
          mYaw += y;
          mPitch += p;
          mRoll += r;
          mChanged = true;
          return *this;
       }

       /// Get a vector pointing forwards.
       inline Vector3 forward() const { return toQuaternion() * Vector3::NEGATIVE_UNIT_Z; }

       /// Get a vector pointing to the right.
       inline Vector3 right() const { return toQuaternion() * Vector3::UNIT_X; }

       /// Get a vector pointing up.
       inline Vector3 up() const { return toQuaternion() * Vector3::UNIT_Y; }

       /**
       @brief Calculate the quaternion of the euler object.
       @details The result is cached, it is only recalculated when the component euler angles are changed.
       */
       inline Quaternion toQuaternion() const
       {
          if(mChanged)
          {
             mCachedQuaternion = Quaternion(mYaw, Vector3::UNIT_Y) * Quaternion(mPitch, Vector3::UNIT_X) * Quaternion(mRoll, Vector3::UNIT_Z);
             mChanged = false;
          }
          return mCachedQuaternion;
       }

       /// Casting operator. This allows any ogre function that wants a Quaternion to accept a Euler instead.
       inline operator Quaternion() const
       {
          return toQuaternion();
       }

       /**
       @brief Calculate the current euler angles of a given quaternion object.
       @param quaternion Quaternion which is used to calculate current euler angles.
       */
       inline Euler &fromQuaternion(const Quaternion &quaternion)
       {
          Matrix3 rotmat;
          quaternion.ToRotationMatrix(rotmat);
          fromMatrix3(rotmat);
          return *this;
       }

       /**
       @brief Calculate the current euler angles of a given matrix object.
       @param matrix Matrix3 which is used to calculate current euler angles.
       */
       inline Euler &fromMatrix3(const Matrix3 &matrix)
       {
          matrix.ToEulerAnglesYXZ(mYaw,mPitch,mRoll);
          mChanged = true;
          return *this;
       }

       /**
       @brief Set the yaw and pitch to face in the given direction.
       @details The direction doesn't need to be normalised. Roll is always unaffected.
       @param setYaw If false, the yaw isn't changed.
       @param setPitch If false, the pitch isn't changed.
       */
       inline Euler &direction(const Vector3 &v, bool setYaw = true, bool setPitch = true)
       {
          Vector3 d(v.normalisedCopy());
          if(setPitch)
             mPitch = Math::ASin(d.y);
          if(setYaw)
             mYaw = Math::ATan2(-d.x, -d.z);
          mChanged = setYaw||setPitch;
           return *this;
       }

       /**
       @brief Normalise the selected rotations to be within the +/-180 degree range.
       @details The normalise uses a wrap around, so for example a yaw of 360 degrees becomes 0 degrees, and -190 degrees becomes 170.
       @param normYaw If false, the yaw isn't normalized.
       @param normPitch If false, the pitch isn't normalized.
       @param normRoll If false, the roll isn't normalized.
       */
       inline Euler &normalise(bool normYaw = true, bool normPitch = true, bool normRoll = true)
       {
            if(normYaw)
              wrapAngle( mYaw );

          if(normPitch)
              wrapAngle( mPitch );

          if(normRoll)
              wrapAngle( mRoll );

          return *this;
       }

       /**
       @brief Return the relative euler angles required to rotate from the current forward direction to the specified dir vector.
       @details The result euler can then be added to the current euler to immediately face dir.
       The rotation won't flip upside down then roll instead of a 180 degree yaw.
       @param setYaw If false, the angle is set to 0. If true, the angle is calculated.
       @param setPitch If false, the angle is set to 0. If true, the angle is calculated.
       @param shortest If false, the full value of each angle is used. If true, the angles are normalised and the shortest
       rotation is found to face the correct direction. For example, when false a yaw of 1000 degrees and a dir of
       (0,0,-1) will return a -1000 degree yaw. When true, the same yaw and dir would give 80 degrees (1080 degrees faces
       the same way as (0,0,-1).
       */
       inline Euler rotationTo(const Vector3 &dir, bool setYaw = true, bool setPitch = true, bool shortest = true) const
       {
          Euler t1;
          Euler t2;
          t1.direction(dir, setYaw, setPitch);
          t2 = t1 - *this;
          if(shortest && setYaw)
          {
             t2.normalise();
          }
          return t2;
       }

       /// Clamp the yaw angle to a range of +/-limit.
       inline Euler &limitYaw(const Radian &limit)
       {
          limitAngle( mYaw, limit );
          return *this;
       }

       /// Clamp the pitch angle to a range of +/-limit.
       inline Euler &limitPitch(const Radian &limit)
       {
          limitAngle( mPitch, limit );
          return *this;
       }

       /// Clamp the roll angle to a range of +/-limit.
       inline Euler &limitRoll(const Radian &limit)
       {
          limitAngle( mRoll, limit );
          return *this;
       }

       /// Stream operator, for printing the euler component angles to a stream
       inline friend std::ostream &operator<<(std::ostream &o, const Euler &e)
       {
          o << "<Y:" << e.mYaw << ", P:" << e.mPitch << ", R:" << e.mRoll << ">";
          return o;
       }

       /// Add two euler objects.
       inline Euler operator+(const Euler &rhs) const
       {
          return Euler(mYaw + rhs.mYaw, mPitch + rhs.mPitch, mRoll + rhs.mRoll);
       }

       /**
       @brief Subtract two euler objects.
       @details This finds the difference as relative angles.
       */
        inline Euler operator-(const Euler &rhs) const
       {
          return Euler(mYaw - rhs.mYaw, mPitch - rhs.mPitch, mRoll - rhs.mRoll);
       }

       /// Interpolate the euler angles by rhs.
       inline Euler operator*(Real rhs) const
       {
          return Euler(mYaw * rhs, mPitch * rhs, mRoll * rhs);
       }

       /// Interpolate the euler angle by lhs.
       inline friend Euler operator*(Real lhs, const Euler &rhs)
       {
          return Euler(lhs * rhs.mYaw, lhs * rhs.mPitch, lhs * rhs.mRoll);
       }

       /**
       @brief Multiply two eulers.
       @details This has the same effect as multiplying quaternions.
       @returns The result is a quaternion.
       */
       inline Quaternion operator*( Euler rhs) const
       {
          Euler e1(*this), e2(rhs);
          return e1.toQuaternion()*e2.toQuaternion();
       }


       /// Apply the euler rotation to the vector rhs.
       inline Vector3 operator*(const Vector3 &rhs) const
       {
          return toQuaternion() * rhs;
       }


       /// Copy assignment operator (Euler)
       inline Euler& operator=(const Euler& src)
       {
          orientation(src.yaw(), src.pitch(), src.roll());
          return *this;
       }

       /// Copy assignment operator (Quaternion)
       inline Euler& operator=(const Quaternion &quaternion)
       {
          fromQuaternion(quaternion);
          return *this;
       }

       /// Copy assignment operator (Matrix3)
       inline Euler& operator=(const Matrix3 &matrix)
       {
          fromMatrix3(matrix);
          return *this;
       }

        inline friend bool operator==( const Euler& left, const Euler& right )
        {
            return left.mYaw   == right.mYaw
                && left.mPitch == right.mPitch
                && left.mRoll  == right.mRoll
            ;
        }

        inline friend bool operator!=( const Euler& left, const Euler& right )
        {
            return !( left == right );
        }

        inline friend bool sameOrientation( const Euler& left, const Euler& right )
        {
            // I'm comparing resulting vectors to avoid having to compare angles that are the same but in different values.
            // Only the resulting oriented vectors really have any meaning in the end.
            return left.forward().positionEquals( right.forward() )
                && left.up().positionEquals( right.up() )
                ;
        }

    protected:
       Radian mYaw;                           //!< Rotation around the Y axis.
       Radian mPitch;                            //!< Rotation around the X axis.
       Radian mRoll;                           //!< Rotation around the Z axis.
       mutable Quaternion mCachedQuaternion;       //!< Cached quaternion equivalent of this euler object.
       mutable bool mChanged;                  //!< Is the cached quaternion out of date?

        inline void wrapAngle( Radian& angle )
        {
            Real rangle = angle.valueRadians();
         if(rangle < -Math::PI)
         {
            rangle = fmod(rangle, -Math::TWO_PI);
            if(rangle < -Math::PI)
            {
               rangle += Math::TWO_PI;
            }
            angle = rangle;
            mChanged = true;
         }
         else if(rangle > Math::PI)
         {
            rangle = fmod(rangle, Math::TWO_PI);
            if(rangle > Math::PI)
            {
               rangle -= Math::TWO_PI;
            }
            angle = rangle;
            mChanged = true;
         }

        }

        inline void limitAngle( Radian& angle, const Radian& limit )
        {
             if(angle > limit)
          {
             angle = limit;
             mChanged = true;
          }
          else if(angle < -limit)
          {
             angle = -limit;
             mChanged = true;
          }
        }
    };

}

#endif
