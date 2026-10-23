# Network input: everything the client can be fed

What reaches the stock client from outside, by channel, and which of it can
carry server-chosen text, numbers or pictures. Compiled 2026-10-16 from the
client binary's strings (`strings -el`/`-a` on DreadGame-Win64-Shipping.exe)
and this repo's handlers. A name listed here means the client knows it; it does
NOT mean the payload shape is known -- see `dreadnought-mmog-responses` before
sending anything new.

## Channels

| Channel | Transport | Served by | Carries |
| --- | --- | --- | --- |
| Launcher tiles, server status | HTTPS :443 `legacyapi.*` | legacy-api | launcher news tiles (`/v2/dreadnought/launcher/dn/tiles/`), age consent, status |
| Login, profile | HTTPS :443 `profile-api.*` | auth-server | JWT, account |
| Server browser | HTTPS :443 `masterserver.*` | master-server | server list |
| Greybox web services | HTTPS :65443 | mmogbrain gateway socket | login/session, `/api/v1/play`, catalog (`digital_items_vc`), bundles, legal docs, **market pictures** |
| Firmament | TLS :48843 | mmogbrain | social/presence/chat + binary YMmog `YA_*` messages |
| Battle server | UDP 7777-7877 (UE4 netdriver) | DreadGame exe headless + host mod | actor replication, replicated properties, `Client*` RPCs |
| Picture fetch | HTTP(S) GET to any URL the catalog names | gateway `/market-images/` | PNGs |

## Pictures from the server: yes, in specific places

The client renders remote images only where its own code reads an image URL:

- Store offers: `full_image_url`, `thumbnail_image_url` (offer converter
  0x2A7F520), `ImageURL`; item data `ImgUrlS`/`ImgUrlM`/`ImgUrlL`
  (FYItemData; also read at 0x2A58A6E, 0x2A6D020). Working today: mmogbrain
  serves decoded hero icons at `/market-images/` (`mmogbrain/market_images.go`,
  `gateway_catalog.go` ~1186). Any PNG put there and named by a catalog entry
  shows in the store.
- A `DownloadImage` / `DreadDownloadImageDelegate` path exists (async web image
  -> texture); which widgets besides the store use it is not traced.
- `backgroundImageUrl` string exists; consumer not found by direct LEA
  (likely built through reflection or a struct property). Unverified.
- In-match announcements (`BP_ShowAnnouncement`, `EYAnnouncementLevel`
  GENERIC/WARNING, `EYAnnouncementType`) take text and an enum, not a URL.
- Promotion popups (`AYMenu::CheckForPromotionItemsNotification`,
  `allowPromotions`) pick store items, so they would show catalog pictures.
  Payload unverified.

## Firmament `YA_*` messages

196 names in the client. 130 are handled or sent by mmogbrain:

```
YA_AchievementsUpdated YA_AddItems YA_AddShipDefaultLoadouts YA_AddToFleet
YA_AnalyticsBeginTransaction YA_AnalyticsButtonClicked
YA_AnalyticsEndTransaction YA_AnalyticsEvent YA_AnalyticsInvestEvent
YA_AnalyticsOnboardingMovie YA_AnalyticsReceiveCreditsEvent
YA_AnalyticsReceiveXPEvent YA_AnalyticsTutorialEvent
YA_AnalyticsTutorialSummaryEvent YA_AnalyticsUpdateTransaction YA_ChargeFleet
YA_Chat YA_ChatAwayChange YA_ChatAwayRemovalRequest YA_ChatJoinRequest
YA_ChatMergeRequest YA_ChatStatus YA_CheckReturn YA_ClaimCareerGoal
YA_ClaimItem YA_Connect YA_ContractRefresh YA_ContractRemove
YA_ContractReplace YA_ConvertShipXP YA_CustomRoomAnalyticsInvite
YA_CustomRoomCancelMatchCountdown YA_CustomRoomChangeHost
YA_CustomRoomChangeHostResponse YA_CustomRoomChangeSettings
YA_CustomRoomChangeSettingsResponse YA_CustomRoomCreate
YA_CustomRoomCreateResponse YA_CustomRoomEnterFleetSelect
YA_CustomRoomEnterFleetSelectResponse YA_CustomRoomExitFleetSelect
YA_CustomRoomExitFleetSelectResponse YA_CustomRoomInvite
YA_CustomRoomStartMatch YA_CustomRoomStartMatchCountdown
YA_CustomRoomStartMatchResponse YA_CustomRoomUpdate
YA_CustomRoomUpdateResponse YA_CustomRoomUserJoin
YA_CustomRoomUserJoinResponse YA_CustomRoomUserLeave
YA_CustomRoomUserLeaveResponse YA_CustomRoomUserRemove YA_CustomRoomUserReturn
YA_CustomRoomUserReturnResponse YA_CustomRoomUserSwitchTeam
YA_CustomRoomUserSwitchTeamResponse YA_EnterMatchmaking YA_FleetAutoRepair
YA_FleetEligibility YA_FleetUpdate YA_GameModeEvent YA_GetBoosterData
YA_GetCareerProgression YA_GetDailyContractsData YA_GetFeatureToggle
YA_GetGameConfigData YA_GetPlayerProgression YA_GetPlayerPurchases
YA_GetPlayerScores YA_GetPlayersInformation YA_GetPlayerStatistics
YA_GetPlayerStatsCounterData YA_GetProgressionData YA_GetScoringData
YA_GetSeasonData YA_GetSeasonProgress YA_GetStaticCareerData YA_GetTechTree
YA_GlobalChat YA_IncrementPlayerStatsCounter YA_LanguageChat
YA_LeaveMatchmaking YA_LeftQueue YA_LogSpecial YA_MembershipChanged
YA_OnFleetCharged YA_PlayAgain YA_PlayerFleets YA_PlayerGet
YA_PlayerStateInHangar YA_PurchaseItem YA_PushInventory YA_QueryRooms
YA_ReconnectJoinChannels YA_RefreshPlayerProfile YA_RemoveFromFleet
YA_RemoveItems YA_RenameShipLoadout YA_RepairFleet YA_RequestStaticFleetData
YA_RequeuingRoomStart YA_RewardCurrencies YA_RoomReturn YA_RoomStart
YA_SaveCtAData YA_SaveGame YA_SavePlayerDisplayInformation YA_ServerShutdown
YA_ServerStarting YA_SetFleetFlagship YA_SquadAccept YA_SquadCreateAndJoin
YA_SquadEliteStatusUpdate YA_SquadEnterMatchmaking YA_SquadInfo YA_SquadInvite
YA_SquadJoin YA_SquadJoined YA_SquadLeave YA_Tune YA_TuneReturn YA_UnlockItem
YA_UpdateFleetMaintenance YA_UpdateGameModes YA_UpdateShipLoadout YA_UserLogin
YA_UserLogout YA_UserOnline YA_UserStatus
```

Known to the client, not implemented (52; analytics omitted below):

```
YA_AdminCmd YA_Campaign YA_CareerGoalsUpdated YA_Catalog
YA_ChangeReputationGoalID YA_ChatInformation YA_ChatValueSet
YA_ClearStatCounters YA_CSATOnlineDisconnect YA_CustomRoom
YA_CustomRoomCancelMatchCountdownResponse YA_CustomRoomInvitePush
YA_CustomRoomReadyForRejoin YA_CustomRoomStartMatchCountdownResponse
YA_Debug_AddMembership YA_DebugFleetRatings YA_DisconnectClient YA_Error
YA_FinalizePurchase YA_FriendOnline YA_GetDashboardManifest
YA_GetLeaderboardData YA_GetMarketManifest YA_GetOffers YA_GetPaywallUrl
YA_IdleKick YA_LeftRequeuingRoom YA_LeftRoom YA_LegalAccept YA_LegalCheck
YA_QueueDisbanded YA_QueueWaitingTime YA_RequeueRoomTimer
YA_RequeueRoomUpdated YA_RequeuingExpired YA_RetrievedMinUsers
YA_SaveMatchmakingParameters YA_SaveOfficer YA_SeasonDataUpdated
YA_SessionActive YA_SessionExpired YA_SessionIdle YA_SessionSuspended
YA_SetPPF YA_SquadInviteInvalidate YA_StageAssessment YA_StagePreparation
YA_Store YA_Test YA_UpdateContract YA_ValidateName YA_WipeLoadouts
```

Pushes in that list that could be useful server-side tools (payloads
unknown): `YA_ChatInformation`, `YA_ChatValueSet`, `YA_SeasonDataUpdated`,
`YA_CareerGoalsUpdated`, `YA_DisconnectClient`, `YA_IdleKick`,
`YA_SessionExpired`, `YA_Error`, `YA_CustomRoomInvitePush`, `YA_FriendOnline`,
`YA_GetDashboardManifest`, `YA_GetMarketManifest`, `YA_GetPaywallUrl`.

Client-to-server analytics (ignorable, 14): CaptainAppearanceChanged, ClientDisconnected, CtALinkClicked, CtAShown, CtATriggered, GoalShowEvent, ItemUnlocks, ModulePreviewMovieOptionToggled, ModulePreviewMoviePlayed, PaywallEvent, ReceiveReputationEvent, StoreEvent, TutorialDeathEvent, ViewMarketItem.

mmogbrain also uses names the client binary does not contain as a literal
(composed at runtime or aliases); see `TestPayloadSizesVerify` for the
authoritative sent set.

## Battle server -> client RPCs

150 `Client*` UFunctions (any replicated actor class; engine ones
included). The battle server is ours to mod, so any of these can be called on a
player's controller/pawn/state through ProcessEvent (`CallFilled` in the host
mod). Ones relevant to server-side effects:

- Text: `ClientMessage` (console/chat line), `ClientReceiveLocalizedMessage`,
  `ClientShowDebugMessage`, `ClientDrawnDebugString`, `ClientNotifyKill`.
- Feel: `ClientPlaySound`, `ClientPlaySoundAtLocation`, `ClientPlayCameraShake`,
  `ClientPlayCameraAnim`, `ClientSpawnCameraLensEffect`,
  `ClientClearCameraLensEffects`, `ClientSetCameraFade`, `ClientPlayForceFeedback`.
- Control: `ClientSetViewTarget`, `ClientSetLocation`, `ClientSetRotation`,
  `ClientIgnoreMoveInput`, `ClientIgnoreLookInput`, `ClientSetCinematicMode`,
  `ClientReturnToMainMenu`, `ClientExecuteCommand` (console command on the
  client; what shipping builds allow is untested).
- Game: `ClientSetTopPlayerMatchStats` (in use), `ClientReceiveRandSelectedUpgrades`
  and `ClientReceiveBoostPurchaseResult` (in use, Havoc), `ClientBossEnemySpawned`,
  `ClientBossEnemyDied`, `ClientHavocStateChanged`, `ClientSetRespawntime`,
  `ClientSetPlayerRestrictions`.

Full list:

```
ClientAckGoodMove ClientActivateMine ClientActors ClientAddAchievement
ClientAddTextureStreamingLoc ClientAdjustPosition
ClientAdjustRootMotionPosition ClientAdjustRootMotionSourcePosition
ClientApplyRespawnFilter ClientAuthorativePosition ClientBaseBoneName
ClientBeaconActorClass ClientBossEnemyDied ClientBossEnemySpawned
ClientCancelPendingMapChange ClientCancelReservationResponse ClientCap
ClientCapBandwidth ClientCapturingCapturePoint ClientCheatFly ClientCheatGhost
ClientCheatWalk ClientClearCameraLensEffects ClientCommitMapChange
ClientConnections ClientCooldownCompleted ClientCycles
ClientDisableShipSelection ClientDrawnDebugBox ClientDrawnDebugString
ClientEnableCharacterSelection ClientEnableNetworkVoice ClientEndOnlineSession
ClientEnemyGotHit ClientEnergyDepleted ClientErrorUpdateRateLimit
ClientEventResult ClientExecuteCommand ClientExplodeMine
ClientFlushLevelStreaming ClientForceGarbageCollection ClientGameEnded
ClientGotoState ClientHavocPlayersChanged ClientHavocStateChanged
ClientIgnoreLookInput ClientIgnoreMoveInput ClientLateJoinInMatch
ClientLateJoinWaitingForNewRound ClientLeavingMatch ClientLoc
ClientMakeSureOrbitTransitionEnded ClientMatchOver ClientMessage
ClientMovementBase ClientMovementMode ClientMutePlayer ClientNewRoundStarts
ClientNotifyActivation ClientNotifyDamageLocation ClientNotifyDeactivation
ClientNotifyKill ClientOnConnected ClientOnQuestCompleted
ClientOnQuestProgression ClientOnScore ClientOnStartReloadActiveWeaponGroup
ClientOutdated ClientPing ClientPlayCameraAnim ClientPlayCameraShake
ClientPlayForceFeedback ClientPlaySound ClientPlaySoundAtLocation
ClientPostRoundEnd ClientPrepareMapChange ClientPreRoundStart
ClientPrestreamTextures ClientReceiveBoostPurchaseResult ClientReceiveData
ClientReceiveLocalizedMessage ClientReceiveProjectilesInRange
ClientReceiveRandSelectedUpgrades ClientReliableBufferOverflow
ClientRemoveRespawnFilter ClientRepObjRef ClientRequestOTSBunch
ClientReservationResponse ClientReset ClientRespawnTeamElimination
ClientRestart ClientRetryClientRestart ClientReturnToMainMenu
ClientReturnToOutpostFromMatchACK ClientRoll ClientRootMotionParams
ClientRunTimerForForcedRespawn ClientSendReservationFull
ClientSendReservationUpdates ClientSendShortCommand ClientSetActiveBoosterIDs
ClientSetActiveFleetId ClientSetBlockOnAsyncLoading ClientSetCameraFade
ClientSetCameraMode ClientSetCinematicMode ClientSetCreepCPU
ClientSetEliteSharePlayers ClientSetForceMipLevelsToBeResident ClientSetHUD
ClientSetLocation ClientSetMatchSoakTestData ClientSetPlayerRestrictions
ClientSetRespawntime ClientSetRespawnWavetime ClientSetRotation
ClientSetSpectatorWaiting ClientSetTopPlayerMatchStats ClientSetViewTarget
ClientShowDebugMessage ClientSpawnCameraLensEffect ClientSpawningInGame
ClientSpawnNearActorResult ClientStartEndOfMatchTransition ClientStartMatch
ClientStartOnlineSession ClientStopCameraAnim ClientStopCameraShake
ClientStopForceFeedback ClientTeamEliminationPlayersChanged ClientTeamMessage
ClientTeleportIntoLevel ClientTravel ClientTravelFailure ClientTravelInternal
ClientTriggerDamageFeedback ClientTriggerPulseVisualAndSoundActorList
ClientUnmutePlayer ClientUpdateActiveWaveModifier ClientUpdateHighscore
ClientUpdateLevelStreamingStatus ClientUpdatePlayerReadyForNextWave
ClientUpdateRewards ClientUpdateTimeToNextWave ClientUpdateWaveProgress
ClientUpdateWeeklyScoreResetTime ClientVeryShortAdjustPosition
ClientVoiceHandshakeComplete ClientWaitForRoundToEnd ClientWasKicked
```

Besides RPCs, every replicated property on the game state, player states,
pawns and components is input too (e.g. the Havoc wave clock in m_matchTime,
progress fields read by ClientUpdateWaveProgress).
